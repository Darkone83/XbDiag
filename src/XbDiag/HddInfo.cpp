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
//   0x00-0x11   HMAC/confounder      (18 bytes)
//   0x12-0x21   HDD key              (16 bytes)
//   0x22-0x31   Console key seed     (16 bytes)
//   0x32-0x33   Region flags         (2 bytes)
//   0x34-0x3F   Serial number        (12 bytes ASCII)
//   0x40-0x47   Online key           (8 bytes)
//   0xC0-0xCF   DVD zone / misc      (varies)
//
// Region decode:
//   Bits 0-7 of word at 0x32:
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

#define ATA_CMD_IDENTIFY 0xEC

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
    BYTE  consoleKey[16];
    BYTE  onlineKey[8];
    BYTE  regionByte;
    char  regionStr[24];
    char  serialEEPROM[14];   // 12 chars + null

    // Export
    bool  exportDone;
    bool  exportOK;
};

static HddData s_data;
static WORD    s_prevBtns;
static bool    s_loaded;

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
        d.eepromOK = (eeStatus == 0 && eeLen >= 0xF0);

        if (d.eepromOK)
        {
            // HDD key: bytes 0x12-0x21 (16 bytes)
            for (int i = 0; i < 16; ++i) d.hddKey[i] = eepBuf[0x12 + i];
            // Console key: bytes 0x22-0x31 (16 bytes)
            for (int i = 0; i < 16; ++i) d.consoleKey[i] = eepBuf[0x22 + i];
            // Online key: bytes 0x40-0x47 (8 bytes)
            for (int i = 0; i < 8; ++i) d.onlineKey[i] = eepBuf[0x40 + i];

            // Region byte: 0x32
            d.regionByte = eepBuf[0x32];
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

    FormatHex(d.consoleKey, 16, hexbuf, sizeof(hexbuf));
    StrCopy(line, sizeof(line), "Console Key:    ");
    StrCat2(line, sizeof(line), line, hexbuf);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    FormatHex(d.onlineKey, 8, hexbuf, sizeof(hexbuf));
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
// OnEnter
// ============================================================================

void HddInfo_OnEnter()
{
    s_prevBtns = 0;
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
        ? (s_data.exportOK ? "[A] Exported OK    [B] Back"
            : "[A] Export failed  [B] Back")
        : "[A] Export    [B] Back";

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

    // Console key (16 bytes = 2 rows)
    DrawText(COL_R, y2, "CONSOLE KEY", 1.2f, COL_YELLOW);
    HLine(y2 + LH + 1.f, COL_R, SW - LM, COL_BORDER);
    y2 += LH + 5.f;
    DrawKeyRow(COL_R, COL_VR, y2, "BYTES   :", d.consoleKey, 16, d.eepromOK);
    y2 += LH * 2.f + GAP;

    // Online key (8 bytes = 1 row)
    DrawText(COL_R, y2, "ONLINE KEY", 1.2f, COL_YELLOW);
    HLine(y2 + LH + 1.f, COL_R, SW - LM, COL_BORDER);
    y2 += LH + 5.f;
    DrawKeyRow(COL_R, COL_VR, y2, "BYTES   :", d.onlineKey, 8, d.eepromOK);
    y2 += LH + GAP;


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
    if (EdgeDown(cur, s_prevBtns, BTN_A))
        ExportData();

    s_prevBtns = cur;
    Render(logo);
}