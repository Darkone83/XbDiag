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

// Secondary ATA channel — DVD drive on Xbox
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

    // Partitions (E/F/G only — X/Y/Z cache skipped)
    struct PartInfo {
        char   letter;
        bool   present;
        char   totalStr[12];   // "XX.X GB"
        char   freeStr[12];    // "XX.X GB"
    } parts[3];  // [0]=E [1]=F [2]=G

    // Export
    bool  exportDone;
    bool  exportOK;

    // SMART
    bool  smartSupported;    // word 82 bit 0 from IDENTIFY
    bool  smartOK;           // SMART READ DATA succeeded
    bool  smartExportDone;
    bool  smartExportOK;
    BYTE  smartBuf[512];     // raw SMART READ DATA response

    // DVD drive (secondary ATA channel, ATAPI IDENTIFY)
    bool  dvdDetected;
    char  dvdModel[44];      // model string from IDENTIFY word[27..46]
};

// View toggle — Info (drive + EEPROM) or SMART attribute table
enum HddView { VIEW_INFO = 0, VIEW_SMART, VIEW_BENCH, VIEW_DVD_BENCH };

static HddData s_data;
static WORD    s_prevBtns;
static bool    s_loaded;
static HddView s_view;

#define BENCH_FILE       "E:\\xbdiag_bench.tmp"
#define BENCH_FILE_SIZE  (8 * 1024 * 1024)   // 8 MB write test
#define BENCH_CHUNK      (64 * 1024)          // 64 KB per tick
#define BENCH_SEEK_ITERS 256

enum BenchState
{
    BENCH_IDLE = 0,
    BENCH_WRITE,
    BENCH_READ,
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

    // Read
    HANDLE      hRead;
    DWORD       readTotal;
    DWORD       readT0;
    float       readMBs;            // result

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
};

static BenchData   s_bench;
static BYTE        s_benchBuf[BENCH_CHUNK];  // static — not stack

// ---- DVD benchmark ----
#define DVD_BENCH_SIZE   (16 * 1024 * 1024)  // 16 MB sequential read
#define DVD_BENCH_CHUNK  (64 * 1024)          // 64 KB per tick
// D:\ is the DVD drive on Xbox (kernel maps \Device\Cdrom0 -> D: at boot)
#define DVD_DRIVE        "D:\\"

enum DvdBenchState { DVD_IDLE = 0, DVD_WAITING, DVD_READING, DVD_DONE };

struct DvdBenchData
{
    DvdBenchState state;
    HANDLE        hFile;
    DWORD         readTotal;
    DWORD         readT0;
    float         readMBs;
    char          statusMsg[80];
};

static DvdBenchData s_dvdBench;
static BYTE         s_dvdBuf[DVD_BENCH_CHUNK];

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

// Probe secondary ATA channel (0x170) for an ATAPI device (DVD drive).
// Sends IDENTIFY PACKET DEVICE (0xA1). Returns true if an ATAPI device
// responds. Word[0] bits[14:8] = 0x85 confirms CD/DVD type.
static bool AtaIdentifyDvd(WORD buf[256])
{
    // Select master on secondary channel
    BYTE devsel = 0xA0;
    __asm { mov dx, ATA2_REG_DEVICE }
    __asm { mov al, devsel          }
    __asm { out dx, al              }

    // 5 status reads (400ns settle)
    for (int i = 0; i < 5; ++i)
    {
        BYTE dummy = 0;
        __asm { mov dx, ATA2_REG_STATUS }
        __asm { in  al, dx              }
        __asm { mov dummy, al           }
    }

    // Wait for not-busy (2s timeout)
    DWORD t0 = GetTickCount();
    for (;;)
    {
        BYTE sr = 0;
        __asm { mov dx, ATA2_REG_STATUS }
        __asm { in  al, dx              }
        __asm { mov sr, al              }
        if (!(sr & ATA_SR_BSY)) break;
        if ((GetTickCount() - t0) > 2000) return false;
        KeStallExecutionProcessor(100);
    }

    // Issue IDENTIFY PACKET DEVICE (0xA1)
    BYTE cmd = 0xA1;
    __asm { mov dx, ATA2_REG_CMD }
    __asm { mov al, cmd          }
    __asm { out dx, al           }

    // Wait for DRQ
    t0 = GetTickCount();
    bool drq = false;
    while ((GetTickCount() - t0) < 3000)
    {
        BYTE sr = 0;
        __asm { mov dx, ATA2_REG_STATUS }
        __asm { in  al, dx              }
        __asm { mov sr, al              }
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
        __asm { mov dx, ATA2_REG_DATA }
        __asm { in  ax, dx            }
        __asm { mov w, ax             }
        buf[i] = w;
    }

    // Confirm ATAPI: word[0] bits[14:8] should be 0x85 for CD/DVD
    return ((buf[0] >> 8) & 0xFF) == 0x85;
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

    // ---- Partition sizes (E/F/G) ----
    {
        const char* letters = "EFG";
        for (int pi = 0; pi < 3; ++pi)
        {
            d.parts[pi].letter = letters[pi];
            d.parts[pi].present = false;

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
            }
        }
    }

    d.exportDone = false;
    d.exportOK = false;

    // ---- DVD drive (secondary ATA channel, ATAPI IDENTIFY) ----
    d.dvdDetected = false;
    StrCopy(d.dvdModel, sizeof(d.dvdModel), "Not detected");
    {
        WORD dvdIdent[256];
        ZeroMemory(dvdIdent, sizeof(dvdIdent));
        if (AtaIdentifyDvd(dvdIdent))
        {
            d.dvdDetected = true;
            AtaStr(&dvdIdent[27], 20, d.dvdModel, sizeof(d.dvdModel));
            if (d.dvdModel[0] == '\0')
                StrCopy(d.dvdModel, sizeof(d.dvdModel), "ATAPI DVD Drive");
        }
    }

    // Init DVD bench state
    ZeroMemory(&s_dvdBench, sizeof(s_dvdBench));
    s_dvdBench.state = DVD_IDLE;
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
            ? (s_data.dvdDetected
                ? "[A] Saved OK    [Right] SMART  [RT] Bench  [LT] DVD  [B] Back"
                : "[A] Saved OK    [Right] SMART  [RT] Bench  [B] Back")
            : (s_data.dvdDetected
                ? "[A] Save failed [Right] SMART  [RT] Bench  [LT] DVD  [B] Back"
                : "[A] Save failed [Right] SMART  [RT] Bench  [B] Back"))
        : (s_data.dvdDetected
            ? "[A] Save hddinfo.txt  [Right] SMART  [RT] Bench  [LT] DVD  [B] Back"
            : "[A] Save hddinfo.txt  [Right] SMART  [RT] Bench  [B] Back");

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
        for (int pi = 0; pi < 3; ++pi)
        {
            const HddData::PartInfo& p = d.parts[pi];
            if (!p.present) continue;
            anyPart = true;
            char label[6]; label[0] = p.letter; label[1] = ':'; label[2] = ' ';
            label[3] = ' '; label[4] = ' '; label[5] = '\0';
            // "X:    12.3 GB  free: 8.1 GB"
            char val[40];
            StrCopy(val, sizeof(val), p.totalStr);
            StrCat2(val, sizeof(val), val, "  free: ");
            StrCat2(val, sizeof(val), val, p.freeStr);
            DrawText(COL_L, y1, label, 1.2f, COL_GRAY);
            DrawText(COL_L + 24.f, y1, val, 1.2f, COL_WHITE);
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
    // Try to find the largest readable file on E:\ as fallback read source
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
    s_bench.hRead = CreateFileA(s_bench.readSrc, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
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
        if (s_bench.hWrite != INVALID_HANDLE_VALUE)
        {
            StrCopy(s_bench.readSrc, sizeof(s_bench.readSrc), writePaths[wi]);
            s_bench.tmpExists = true;
            s_bench.writeTotal = 0;
            s_bench.writeT0 = GetTickCount();
            s_bench.state = BENCH_WRITE;
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
    if (s_bench.hRead != INVALID_HANDLE_VALUE && s_bench.hRead != NULL)
    {
        CloseHandle(s_bench.hRead); s_bench.hRead = INVALID_HANDLE_VALUE;
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

    const char* hdr = "XbDiag HDD Benchmark\r\n====================\r\n";
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
        StrCopy(line, sizeof(line), "Write Speed: ");
        StrCat2(line, sizeof(line), line, val);
        StrCat2(line, sizeof(line), line, " MB/s\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);
    }

    FmtFloat(s_bench.readMBs, val, sizeof(val));
    StrCopy(line, sizeof(line), "Read Speed:  ");
    StrCat2(line, sizeof(line), line, val);
    StrCat2(line, sizeof(line), line, " MB/s\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

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

// One tick of benchmark work — called every frame during VIEW_BENCH
static void BenchTick()
{
    switch (s_bench.state)
    {
    case BENCH_WRITE:
    {
        DWORD toWrite = BENCH_CHUNK;
        DWORD remaining = BENCH_FILE_SIZE - s_bench.writeTotal;
        if (toWrite > remaining) toWrite = remaining;

        DWORD written = 0;
        WriteFile(s_bench.hWrite, s_benchBuf, toWrite, &written, NULL);
        s_bench.writeTotal += written;

        if (s_bench.writeTotal >= (DWORD)BENCH_FILE_SIZE || written == 0)
        {
            CloseHandle(s_bench.hWrite);
            s_bench.hWrite = INVALID_HANDLE_VALUE;

            DWORD elapsed = GetTickCount() - s_bench.writeT0;
            if (elapsed > 0)
                s_bench.writeMBs = (float)s_bench.writeTotal / 1048576.f
                / ((float)elapsed / 1000.f);

            // Move to read phase
            BenchStartRead();
        }
        break;
    }

    case BENCH_READ:
    {
        DWORD bytesRead = 0;
        ReadFile(s_bench.hRead, s_benchBuf, BENCH_CHUNK, &bytesRead, NULL);
        s_bench.readTotal += bytesRead;

        // Stop after 8MB read (same as write size)
        if (bytesRead == 0 || s_bench.readTotal >= (DWORD)BENCH_FILE_SIZE)
        {
            CloseHandle(s_bench.hRead);
            s_bench.hRead = INVALID_HANDLE_VALUE;

            DWORD elapsed = GetTickCount() - s_bench.readT0;
            if (elapsed > 0 && s_bench.readTotal > 0)
                s_bench.readMBs = (float)s_bench.readTotal / 1048576.f
                / ((float)elapsed / 1000.f);

            s_bench.state = BENCH_SEEK;

            // Open seek handle (random reads — no sequential flag)
            s_bench.hSeek = CreateFileA(s_bench.readSrc, GENERIC_READ,
                FILE_SHARE_READ, NULL, OPEN_EXISTING,
                FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS, NULL);
            if (s_bench.hSeek == INVALID_HANDLE_VALUE)
            {
                s_bench.seekMs = 0.f;
                s_bench.state = BENCH_DONE;
                // Cleanup temp file
                if (s_bench.tmpExists)
                {
                    DeleteFileA(BENCH_FILE); s_bench.tmpExists = false;
                }
            }
            else
            {
                s_bench.seekIdx = 0;
                s_bench.seekT0 = GetTickCount();
            }
        }
        break;
    }

    case BENCH_SEEK:
    {
        // Do 8 seek+read operations per tick to keep it responsive
        static DWORD seekSeed = 0xDEADBEEF;
        int perTick = 8;
        if (s_bench.seekIdx + perTick > BENCH_SEEK_ITERS)
            perTick = BENCH_SEEK_ITERS - s_bench.seekIdx;

        // Get file size for offset range
        DWORD fileSize = GetFileSize(s_bench.hSeek, NULL);
        if (fileSize < 512) fileSize = 512;
        DWORD range = (fileSize / 512) - 1;

        for (int si = 0; si < perTick; ++si)
        {
            DWORD sector = range > 0 ? (BenchRand(seekSeed) % range) : 0;
            DWORD offset = sector * 512;
            SetFilePointer(s_bench.hSeek, (LONG)offset, NULL, FILE_BEGIN);
            DWORD bytesRead = 0;
            ReadFile(s_bench.hSeek, s_benchBuf, 512, &bytesRead, NULL);
        }
        s_bench.seekIdx += perTick;

        if (s_bench.seekIdx >= BENCH_SEEK_ITERS)
        {
            CloseHandle(s_bench.hSeek);
            s_bench.hSeek = INVALID_HANDLE_VALUE;

            DWORD elapsed = GetTickCount() - s_bench.seekT0;
            if (BENCH_SEEK_ITERS > 0)
                s_bench.seekMs = (float)elapsed / (float)BENCH_SEEK_ITERS;

            // Delete temp file
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
        DrawText(LM, y, "WRITE :", 1.2f, COL_GRAY);
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

    // READ
    DrawText(LM, y, "READ  :", 1.2f, COL_GRAY);
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

    // SEEK
    DrawText(LM, y, "SEEK  :", 1.2f, COL_GRAY);
    if (s_bench.state == BENCH_SEEK)
    {
        char prog[16];
        IntToStr(s_bench.seekIdx, prog, sizeof(prog));
        StrCat2(prog, sizeof(prog), prog, "/256");
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
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// DVD Benchmark
// ============================================================================


static void DvdBenchCleanup();  // forward declaration — defined after DvdBenchTryStart

// Enter the waiting-for-disc state — just resets and shows the prompt
static void DvdBenchStart()
{
    ZeroMemory(&s_dvdBench, sizeof(s_dvdBench));
    s_dvdBench.state = DVD_WAITING;
    s_dvdBench.hFile = INVALID_HANDLE_VALUE;
}

// Called when user presses [A] from DVD_WAITING — mounts and probes disc
static void DvdBenchTryStart()
{
    // Clean up any previous mount
    DvdBenchCleanup();

    // Check disc readiness — GetFileAttributesA on D:\ returns 0xFFFFFFFF if no disc
    DWORD attr = GetFileAttributesA("D:\\");
    if (attr == 0xFFFFFFFF)
    {
        StrCopy(s_dvdBench.statusMsg, sizeof(s_dvdBench.statusMsg),
            "No disc inserted or drive not ready");
        s_dvdBench.state = DVD_DONE;
        return;
    }

    // Find a readable file — try default.xbe first, then enumerate root
    const char* candidates[] = { "D:\\default.xbe", "D:\\default.xex", NULL };
    HANDLE hf = INVALID_HANDLE_VALUE;

    for (int i = 0; candidates[i] && hf == INVALID_HANDLE_VALUE; ++i)
    {
        hf = CreateFileA(candidates[i], GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING,
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    }

    if (hf == INVALID_HANDLE_VALUE)
    {
        WIN32_FIND_DATA fd;
        HANDLE hFind = FindFirstFileA("D:\\*", &fd);
        while (hFind != INVALID_HANDLE_VALUE)
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                fd.nFileSizeLow >= (1024 * 1024))
            {
                char path[64];
                StrCat2(path, sizeof(path), "D:\\", fd.cFileName);
                hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                    NULL, OPEN_EXISTING,
                    FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
                if (hf != INVALID_HANDLE_VALUE) break;
            }
            if (!FindNextFileA(hFind, &fd)) break;
        }
        if (hFind != INVALID_HANDLE_VALUE) FindClose(hFind);
    }

    if (hf == INVALID_HANDLE_VALUE)
    {
        StrCopy(s_dvdBench.statusMsg, sizeof(s_dvdBench.statusMsg),
            "Disc present but no readable file found");
        s_dvdBench.state = DVD_DONE;
        return;
    }

    s_dvdBench.hFile = hf;
    s_dvdBench.readTotal = 0;
    s_dvdBench.readT0 = GetTickCount();
    s_dvdBench.state = DVD_READING;
    StrCopy(s_dvdBench.statusMsg, sizeof(s_dvdBench.statusMsg), "Reading...");
}

static void DvdBenchCleanup()
{
    if (s_dvdBench.hFile && s_dvdBench.hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(s_dvdBench.hFile);
        s_dvdBench.hFile = INVALID_HANDLE_VALUE;
    }
    // No unmount needed — D:\ is the kernel-mapped DVD drive letter
}

static void DvdBenchTick()
{
    if (s_dvdBench.state != DVD_READING) return;

    DWORD got = 0;
    BOOL ok = ReadFile(s_dvdBench.hFile, s_dvdBuf, DVD_BENCH_CHUNK, &got, NULL);

    if (!ok || got == 0)
    {
        // EOF or error — finalise
        DWORD elapsed = GetTickCount() - s_dvdBench.readT0;
        if (elapsed > 0 && s_dvdBench.readTotal > 0)
        {
            float secs = (float)elapsed / 1000.f;
            s_dvdBench.readMBs = ((float)s_dvdBench.readTotal / (1024.f * 1024.f)) / secs;
            StrCopy(s_dvdBench.statusMsg, sizeof(s_dvdBench.statusMsg), "Complete");
        }
        else
        {
            StrCopy(s_dvdBench.statusMsg, sizeof(s_dvdBench.statusMsg),
                !ok ? "Read error" : "File too small");
        }
        DvdBenchCleanup();
        s_dvdBench.state = DVD_DONE;
        return;
    }

    s_dvdBench.readTotal += got;

    // Stop after DVD_BENCH_SIZE
    if (s_dvdBench.readTotal >= DVD_BENCH_SIZE)
    {
        DWORD elapsed = GetTickCount() - s_dvdBench.readT0;
        float secs = elapsed > 0 ? (float)elapsed / 1000.f : 1.f;
        s_dvdBench.readMBs = ((float)s_dvdBench.readTotal / (1024.f * 1024.f)) / secs;
        StrCopy(s_dvdBench.statusMsg, sizeof(s_dvdBench.statusMsg), "Complete");
        DvdBenchCleanup();
        s_dvdBench.state = DVD_DONE;
    }
}

static void RenderDvdBench(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    // Hint bar is state-dependent
    const char* hint = "[B] Back";
    if (s_dvdBench.state == DVD_WAITING)
        hint = "[A] Check disc   [B] Back";
    else if (s_dvdBench.state == DVD_READING)
        hint = "[B] Cancel";
    else if (s_dvdBench.state == DVD_DONE)
        hint = "[A] Try again   [B] Back";

    DrawPageChrome(logo, "HDD INFO", hint);

    const float BX = LM;
    const float BW = SW - LM * 2.f - 10.f;
    float y = CONTENT_Y + 10.f;

    // Title + drive model
    DrawText(BX, y, "DVD READ SPEED TEST", 1.4f, COL_YELLOW);   y += LINE_H + 4.f;
    DrawText(BX, y, s_data.dvdModel, 1.2f, COL_CYAN);           y += LINE_H + 12.f;

    if (s_dvdBench.state == DVD_WAITING)
    {
        DrawText(BX, y, "Insert a disc and press [A]", 1.3f, COL_WHITE);
        y += LINE_H + 6.f;
        DrawText(BX, y, "The drive will be checked for readiness before testing.",
            1.1f, COL_DIM);
    }
    else if (s_dvdBench.state == DVD_READING)
    {
        // Progress bar
        float prog = (float)s_dvdBench.readTotal / (float)DVD_BENCH_SIZE;
        if (prog > 1.f) prog = 1.f;
        float barH = 18.f;
        FillRect(BX, y, BX + BW, y + barH, D3DCOLOR_XRGB(10, 14, 32));
        if (prog > 0.f)
            FillRect(BX, y, BX + BW * prog, y + barH, D3DCOLOR_XRGB(0, 160, 220));
        HLine(y, BX, BX + BW, COL_BORDER);
        HLine(y + barH, BX, BX + BW, COL_BORDER);
        VLine(BX, y, y + barH, COL_BORDER);
        VLine(BX + BW, y, y + barH, COL_BORDER);
        y += barH + 6.f;

        // MB read so far
        char t[32];
        int mbRead = Ftoi((float)s_dvdBench.readTotal / (1024.f * 1024.f));
        int mbTotal = Ftoi((float)DVD_BENCH_SIZE / (1024.f * 1024.f));
        IntToStr(mbRead, t, sizeof(t));
        DrawText(BX, y, t, 1.2f, COL_WHITE);
        DrawText(BX + TW(t, 1.2f), y, " / ", 1.2f, COL_DIM);
        IntToStr(mbTotal, t, sizeof(t));
        DrawText(BX + TW("000 / ", 1.2f), y, t, 1.2f, COL_WHITE);
        DrawText(BX + TW("000 / 00", 1.2f), y, " MB", 1.2f, COL_DIM);
    }
    else // DVD_DONE
    {
        if (s_dvdBench.readMBs > 0.f)
        {
            // Result
            DrawText(BX, y, "READ SPEED", 1.3f, COL_GRAY);  y += LINE_H + 4.f;

            // Large result value
            char mbs[16];
            int whole = Ftoi(s_dvdBench.readMBs);
            int frac = Ftoi((s_dvdBench.readMBs - (float)whole) * 10.f);
            IntToStr(whole, mbs, sizeof(mbs));
            // append .X
            int len = 0; while (mbs[len]) ++len;
            mbs[len++] = '.'; mbs[len++] = '0' + frac; mbs[len] = '\0';

            DrawText(BX, y, mbs, 2.0f, COL_GREEN);
            DrawText(BX + TW(mbs, 2.0f) + 4.f, y + 6.f, "MB/s", 1.3f, COL_WHITE);
            y += LINE_H * 2.f + 8.f;

            // Contextual note — DVD-ROM 1x = 1.385 MB/s
            // Stock Xbox DVD = ~8x CAV (~5-11 MB/s depending on radius)
            DrawText(BX, y, "Ref: 1x DVD = 1.39 MB/s  |  Stock drive ~5-11 MB/s", 1.1f, COL_DIM);
        }
        else
        {
            DrawText(BX, y, s_dvdBench.statusMsg, 1.3f, COL_RED);
        }
        y += LINE_H + 8.f;
        DrawText(BX, y, "[A] Run again   [B] Back", 1.2f, COL_YELLOW);
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
        // Let the bench-specific B handlers below consume this if a bench is active
        bool hddBusy = (s_view == VIEW_BENCH &&
            s_bench.state != BENCH_IDLE && s_bench.state != BENCH_DONE);
        bool dvdBusy = (s_view == VIEW_DVD_BENCH);
        if (!hddBusy && !dvdBusy)
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
        else if (s_view == VIEW_DVD_BENCH)
            DvdBenchCleanup();
        s_view = VIEW_INFO;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_RTRIG) && s_view == VIEW_INFO)
        s_view = VIEW_BENCH;
    if (EdgeDown(cur, s_prevBtns, BTN_LTRIG) && s_view == VIEW_INFO && s_data.dvdDetected)
    {
        s_view = VIEW_DVD_BENCH;
        DvdBenchStart();   // enter waiting-for-disc prompt immediately
    }

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
            else if (s_bench.state == BENCH_DONE)
                ExportBench();
        }
        else if (s_view == VIEW_DVD_BENCH)
        {
            if (s_dvdBench.state == DVD_WAITING)
                DvdBenchTryStart();
            else if (s_dvdBench.state == DVD_DONE)
                DvdBenchStart();   // back to insert-disc prompt
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
    if (s_view == VIEW_DVD_BENCH && s_dvdBench.state == DVD_READING)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
        {
            DvdBenchCleanup();
            s_dvdBench.state = DVD_DONE;
            StrCopy(s_dvdBench.statusMsg, sizeof(s_dvdBench.statusMsg), "Cancelled");
            s_prevBtns = cur;
            RenderDvdBench(logo);
            return;
        }
    }
    // From DVD_WAITING or DVD_DONE, B just goes back to info page
    if (s_view == VIEW_DVD_BENCH && s_dvdBench.state != DVD_READING)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
        {
            DvdBenchCleanup();
            s_view = VIEW_INFO;
            s_prevBtns = cur;
            Render(logo);
            return;
        }
    }

    s_prevBtns = cur;

    // Run one tick of work
    if (s_view == VIEW_BENCH && s_bench.state != BENCH_IDLE && s_bench.state != BENCH_DONE)
        BenchTick();
    if (s_view == VIEW_DVD_BENCH && s_dvdBench.state == DVD_READING)
        DvdBenchTick();

    if (s_view == VIEW_SMART)
        RenderSmart(logo);
    else if (s_view == VIEW_BENCH)
        RenderBench(logo);
    else if (s_view == VIEW_DVD_BENCH)
        RenderDvdBench(logo);
    else
        Render(logo);
}