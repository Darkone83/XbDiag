// SysInfo.cpp
// XbDiag - System Information module
//
// All hardware data read once in OnEnter().
//
// Layout:
//   Left column:  CPU / Memory / Chipset+Revision / Mods
//   Right column: Video / Thermal / Storage / Network
//   Bottom strip: CPU MHz | GPU MHz (live color bar)
//
// CPU section:
//   CPU    (cpuIC — Coppermine-128 / Tualatin-512 / Tualatin-256 (Cel) etc.)
//   Speed  (TSC-measured, matching PrometheOS getCPUFreq exactly)
//   Brand  (CPUID 0x80000002-4 extended brand string)
//   CPUID  (raw leaf 1 EAX hex)
//
//   Speed measurement (MeasureCpuMHz):
//   Matches PrometheOS xboxConfig::getCPUFreq() + RDTSC() exactly:
//     - Each RDTSC in a single __asm {} block — EAX:EDX saved before the
//       compiler can insert code between rdtsc and the stores.  Multiple
//       separate __asm blocks allowed EAX corruption on Tualatin upgrades.
//     - No CPUID serialization fence (PrometheOS doesn't use one)
//     - 64-bit TSC as double: x = hi; x *= 2^32; x += lo  (exact PrometheOS form)
//     - Sleep(300ms) kernel-managed window
//
// Revision detection (aligned with PrometheOS getHardwareRevision):
//   1. PIC reg 0x01 x3 reads  -> 3-char board string (P01/P05/P11/P2L)
//   2. Encoder SMBus probe    -> splits P11 into 1.2/1.3 vs 1.4/1.5
//      Conexant (0x8A) ACK   -> report "1.2/1.3" (NV2A rev byte is NOT
//                               reliable; some 1.3 boards have A1 silicon)
//      Focus (0xD4) ACK      -> chip ID 0x54=FS454->1.4 / 0x09=FS455->1.5
//   3. P2L -> NV2A EMRS strap bits[19:18] -> splits 1.6 vs 1.6b
//      Hynix (strap==3) -> 1.6b    Samsung (strap!=3) -> 1.6
//
// Modchip detection (full PrometheOS modchip list):
//   Xecuter   0xF500 == 0xE1
//   Modxo     0xDEAD == 0xAF
//   Smartxx   0xF701 in {0xF1, 0xF2, 0xF8}
//   Aladdin   0xF701 in {0x11, 0x15} = 1MB  /  0x69 = 2MB
//   Xchanger  0x1912 != 0xFF
//   Xtremium  0x00C  low nibble 1-10  (XTREMIUM_REGISTER_BANKING)
//   Xenium    0x00EF low nibble 1-10  (XENIUM_REGISTER_BANKING)
//
// UDMA detection:
//   Word 53 bit 2 must be set for word 88 to be valid (ATA spec).
//   Xbox MCPX programs DMA mode in its own PCI timing registers without
//   issuing software SET FEATURES — word 88 active bits (14:8) are often
//   zero even though UDMA IS running. If word 53 bit 2 is set and supported
//   bits are non-zero, we report the highest supported UDMA mode (no marker).
//   '?' suffix = word 53 bit 2 clear, word 88 validity not guaranteed.
//
// CPU speed: read from MCPX CPUMPLL (PCI 0:3:0 offset 0x6C) + MSR 0x2A ratio.
// GPU speed: NV2A PRAMDAC NVPLL at MMIO 0xFD680500 - decode M/N/P.
//
// Thermal (cross-revision):
//   1.0-1.5: ADM1032 at SMBus 0x4C
//              reg 0x00 = local die (board ambient)
//              reg 0x01 = remote diode (CPU die)
//   1.6:     ADM1032 absent; use PIC at 0x10
//              reg 0x09 = CPU temp (°C)
//              reg 0x0A = board temp (°C)
//
// [A] Export to D:\sysinfo.txt     [X] Flash chip info     [Y] Dump BIOS     [B] Back
//
// Network: IP via UDP connect trick (no DNS, no blocking).

#include "SysInfo.h"
#include "font.h"
#include "input.h"
#include <xtl.h>
#include <winsockx.h>   // XNetStartup, XNetGetTitleXnAddr, XNADDR

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

// ============================================================================
// Data
// ============================================================================

struct SysData
{
    // CPU
    char cpuBrand[48];
    char cpuIC[24];             // "Coppermine-128"
    char cpuSpeedMHz[16];       // measured via TSC, e.g. "733 MHz"
    DWORD cpuMHz;               // numeric for status bar
    bool  isXemu;               // true if running under xemu/KVM
    char cpuLeaf1[12];          // raw CPUID leaf 1 EAX hex e.g. "0x00000683"

    // Memory
    char memTotal[12];
    char memAvail[12];
    char memConfig[32];

    // Chipset + board revision
    char chipDevId[12];
    char chipRev[8];
    char boardRevPIC[5];        // raw 3-char PIC string + null  (P01/P05/P11/P2L/DBG...)
    char boardRevFinal[16];     // human-readable: "1.0", "1.4/1.5", "1.6b" etc.
    char serialNum[16];         // raw EEPROM serial string
    bool  rtcPresent;           // X-RTC module at SMBus 0x68
    bool  dispPresent;          // I2C display detected at 0x27, 0x3C, or 0x3D
    char  dispAddr[8];          // e.g. "0x3C"
    char  modchipName[20];      // e.g. "Aladdin 1MB", "Modxo", "None/TSOP"
    char  hdModVer[24];         // e.g. "X-HD V255.255.255 BL" (20 chars + null)

    // BIOS
    char biosVer[32];

    // Video
    char encName[28];
    char encId[8];
    char avPack[24];

    // Thermal
    char tempAmbient[10];       // board ambient (ADM1032 or PIC)
    char tempCPU[10];           // CPU die temp  (ADM1032 or PIC)
    bool tempOK;
    BYTE rawTempAmbient;
    BYTE rawTempCPU;

    // Storage
    char hddModel[48];
    char hddSerial[24];
    char hddSizeGB[12];
    char hddUDMA[8];
    bool hddPresent;



    // Network
    char macAddr[20];
    bool macOK;
    char ipAddr[20];    // dotted-decimal local IP
    bool ipOK;

    // GPU speed
    char gpuSpeedMHz[16];
    DWORD gpuMHz;

    // Export
    bool exportDone;
    bool exportOK;

    // BIOS dump
    bool biosDumpDone;
    bool biosDumpOK;
    DWORD biosDumpSize;     // bytes written (256KB or 1MB)
    BYTE biosDumpError;     // 0=ok 1=map fail 2=file fail 3=write fail
    DWORD biosDumpLastErr;  // GetLastError() captured on file open fail
};

// Flash chip detection result — populated once in ReadSysData()
struct FlashInfo
{
    bool  probed;           // JEDEC autoselect was attempted
    bool  found;            // recognized chip in known-ID table
    bool  flashable;        // WE# bridged, known-good command set
    bool  modchipGuard;     // skipped: modchip owns the LPC bus
    bool  rev16Guard;       // skipped: rev 1.6/1.6b has no TSOP
    BYTE  mfrId;            // raw manufacturer byte
    BYTE  devId;            // raw device byte
    char  chipName[32];     // human-readable name or status
    char  sizeStr[8];       // "256KB" / "1MB" / ""
    char  mfrHex[6];        // "0xDA" etc.
    char  devHex[6];        // "0x0B" etc.
};

static SysData   s_data;
static FlashInfo s_flashInfo;
static WORD      s_prevBtns = 0;
static bool      s_dataLoaded = false;
static bool      s_flashPopupOpen = false;

// Forward declaration — defined after MmMapIoSpace extern block below
static void DetectFlashChip(FlashInfo& fi);

// ============================================================================
// Utilities
// ============================================================================

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

// ============================================================================
// PCI config space (mechanism 1)
// ============================================================================

// PCI config space read via kernel export (EXPORTNUM 46).
// HalReadWritePCISpace is the safe arbitrated path — avoids raw 0xCF8/0xCFC
// port I/O which shares the PCI config address register with the kernel.
//   SlotNumber encodes dev[4:0] | func[2:0]<<5  (PCI_SLOT_NUMBER format)
extern "C" VOID __stdcall HalReadWritePCISpace(
    ULONG BusNumber, ULONG SlotNumber, ULONG RegisterNumber,
    PVOID Buffer, ULONG Length, BOOLEAN WritePCISpace);

static DWORD PciRead32(BYTE bus, BYTE dev, BYTE func, BYTE reg)
{
    DWORD val = 0;
    ULONG slot = ((ULONG)dev & 0x1F) | (((ULONG)func & 0x07) << 5);
    HalReadWritePCISpace(bus, slot, reg, &val, sizeof(val), FALSE);
    return val;
}

// ============================================================================
// CPUID
// ============================================================================

static void DoCpuid(DWORD leaf,
    DWORD& reax, DWORD& rebx, DWORD& recx, DWORD& redx)
{
    __asm
    {
        mov  eax, leaf
        cpuid
        mov[reax], eax
        mov[rebx], ebx
        mov[recx], ecx
        mov[redx], edx
    }
}

// ============================================================================
// Pure PLL-based CPU frequency — no MSR, no CPUID, no timing window.
//
// CPUMPLL  offset 0x6C:  FSB divider (byte 0) + multiplier (byte 1)
// CPUCTL   offset 0x68:  bits [21:17] = 5-bit ratio index (bootloader-programmed)
// ============================================================================

static DWORD CpuRatioX10FromCpuctl(DWORD cpuctl)
{
    BYTE idx = (BYTE)((cpuctl >> 17) & 0x1F);
    switch (idx)
    {
    case 0x01: return 30;
    case 0x05: return 35;
    case 0x02: return 40;
    case 0x06: return 45;
    case 0x00: return 50;
    case 0x04: return 55;
    case 0x0B: return 60;
    case 0x0F: return 65;
    case 0x09: return 70;
    case 0x0D: return 75;
    case 0x0A: return 80;
    case 0x16: return 85;
    case 0x10: return 90;
    case 0x14: return 95;
    case 0x1B: return 100;
    case 0x1F: return 105;
    case 0x1A: return 130;
    case 0x1C: return 140;
    default:   return 0;
    }
}

static DWORD MeasureCpuMHz()
{
    DWORD cpumpll = PciRead32(0, 3, 0, 0x6C);
    DWORD cpuctl = PciRead32(0, 3, 0, 0x68);

    DWORD fsb_div = cpumpll & 0xFF;
    DWORD fsb_mult = (cpumpll >> 8) & 0xFF;

    if (fsb_div == 0 || fsb_mult == 0) return 733;

    double fsb_hz = (50000000.0 / 3.0) * ((double)fsb_mult / (double)fsb_div);

    DWORD ratio = CpuRatioX10FromCpuctl(cpuctl);
    if (ratio == 0) return 733;

    double cpu_mhz = (fsb_hz * ((double)ratio / 10.0)) / 1.0e6;
    DWORD  result = (DWORD)(cpu_mhz + 0.5);

    if (result < 400 || result > 1600) return 733;
    return result;
}

// ============================================================================
// NV2A GPU PLL frequency
// PRAMDAC NVPLL register at MMIO base 0xFD000000 + 0x680500
// Bits [7:0]  = M (input divider)
// Bits [15:8] = N (feedback multiplier)
// Bits [19:16]= P (post divider, shift)
// F_out = (crystal * N) / (M * 2^P)   crystal = 16.666 MHz on Xbox
// ============================================================================

static DWORD ReadGpuMHz()
{
    // NV2A MMIO base is at 0xFD000000 on Xbox
    // Guard: verify the NV2A vendor/device via PCI first to confirm
    // the MMIO window is live before dereferencing.
    DWORD pciId = PciRead32(0, 0, 0, 0x00);
    // NV2A: vendor 0x10DE, device 0x02A0
    if ((pciId & 0xFFFF) != 0x10DE)
        return 233;  // not NVIDIA - return safe default

    volatile DWORD* pll =
        (volatile DWORD*)(0xFD000000UL + 0x00680500UL);

    DWORD reg = *pll;
    DWORD M = (reg >> 0) & 0xFF;
    DWORD N = (reg >> 8) & 0xFF;
    DWORD P = (reg >> 16) & 0x0F;

    if (M == 0) return 233;   // fallback - avoid div zero
    if (N == 0) return 233;

    // Crystal = 50000000/3 Hz (16.666... MHz) — exact, matches reference.
    // F_out = (N * crystal) / (M * 2^P)
    // Use double to avoid integer truncation on GPU frequencies.
    double gpu_hz = ((double)N * (50000000.0 / 3.0)) / ((double)M * (double)(1u << P));
    DWORD  mhz = (DWORD)(gpu_hz / 1.0e6 + 0.5);  // round to nearest

    // Sanity check - NV2A should be 200-300 MHz
    if (mhz < 150 || mhz > 400) return 233;
    return mhz;
}

// ============================================================================
// ATA IDENTIFY
// ============================================================================

#define ATA_DATA   0x1F0
#define ATA_DEVSEL 0x1F6
#define ATA_STATUS 0x1F7
#define ATA_CMD    0x1F7
#define ATA_BSY    0x80
#define ATA_DRQ    0x08
#define ATA_ERR    0x01

extern "C" VOID __stdcall KeStallExecutionProcessor(ULONG Microseconds);
extern "C" LONG __stdcall ExQueryNonVolatileSetting(
    ULONG ValueIndex, ULONG* Type, void* Value,
    ULONG ValueLength, ULONG* ResultLength);


static bool AtaWaitReady(int msTimeout)
{
    DWORD t0 = GetTickCount();
    BYTE  status = 0;
    do {
        __asm { mov dx, ATA_STATUS }
        __asm { in  al, dx         }
        __asm { mov status, al     }
        // Only check BSY+DRDY — do NOT abort on ERR here; the ERR bit can be
        // stale from a prior command and must not be tested before IDENTIFY issues.
        if (!(status & ATA_BSY) && (status & 0x40)) return true;
        KeStallExecutionProcessor(1);
    } while ((GetTickCount() - t0) < (DWORD)msTimeout);
    return false;
}

static bool AtaIdentify(WORD buf[256])
{
    // Select master drive
    BYTE sel = 0xA0;  // ATA spec: bits[7,5] fixed, bit[4]=0 for master
    __asm { mov dx, ATA_DEVSEL }
    __asm { mov al, sel        }
    __asm { out dx, al         }

    // Settle: 5 status reads (400ns rule per ATA spec) - not a tight spin
    for (int i = 0; i < 5; ++i)
    {
        BYTE dummy = 0;
        __asm { mov dx, ATA_STATUS }
        __asm { in  al, dx         }
        __asm { mov dummy, al      }
    }

    // Wait for not-busy before issuing command
    if (!AtaWaitReady(2000)) return false;

    // Issue IDENTIFY
    BYTE cmd = 0xEC;
    __asm { mov dx, ATA_CMD }
    __asm { mov al, cmd    }
    __asm { out dx, al     }

    // Wait for DRQ with hard timeout
    DWORD t0 = GetTickCount();
    bool  drq = false;
    while ((GetTickCount() - t0) < 2000)
    {
        BYTE status = 0;
        __asm { mov dx, ATA_STATUS }
        __asm { in  al, dx         }
        __asm { mov status, al     }
        if (status & ATA_BSY) { KeStallExecutionProcessor(100); continue; }
        if (status & ATA_ERR) return false;
        if (status & ATA_DRQ) { drq = true; break; }
        KeStallExecutionProcessor(50);
    }
    if (!drq) return false;

    // Read exactly 256 words
    for (int i = 0; i < 256; ++i)
    {
        WORD w = 0;
        __asm { mov dx, ATA_DATA }
        __asm { in  ax, dx       }
        __asm { mov[w], ax      }
        buf[i] = w;
    }
    return true;
}

static void AtaSwapStr(const WORD* words, int nWords, char* out, int outLen)
{
    int pos = 0;
    for (int i = 0; i < nWords && pos < outLen - 1; ++i)
    {
        out[pos++] = (char)(words[i] >> 8);
        if (pos < outLen - 1)
            out[pos++] = (char)(words[i] & 0xFF);
    }
    out[pos] = '\0';
    for (int i = pos - 1; i >= 0 && out[i] == ' '; --i)
        out[i] = '\0';
}

// ============================================================================
// Board revision detection  (ref: PrometheOS getHardwareRevision)
// ============================================================================
//
// Primary method: read PIC reg 0x01 three times - the PIC16L shifts out one
// byte per read on the same register, giving a 3-char board string:
//   P01 = 1.0
//   P05 = 1.1
//   P11 = 1.2 / 1.3 / 1.4 / 1.5  (disambiguated below)
//   P2L = 1.6 / 1.6b              (disambiguated below)
//   DBG = Debug/Alpha kit   D01 variants = Dev kit
//
// P11 disambiguation:
//   Probe Conexant encoder (SMBADDR_ENC_CNXT 0x8A):
//     ACK -> report "1.2/1.3".  NV2A PCI rev byte is NOT used here because
//            some 1.3 boards ship NV2A-A1 silicon, giving false "1.2" results.
//   Probe Focus encoder (SMBADDR_ENC_FOCUS 0xD4):
//     ACK -> chip ID reg 0x00:
//              0x54 (FS454) -> 1.4    0x09 (FS455) -> 1.5    other -> 1.4/1.5
//   Both NAK -> 1.2/1.3 (fallback)
//
// P2L disambiguation:
//   NV2A EMRS strap bits[19:18] at MMIO 0xFD101000.
//   Hynix RAM (strap==3) -> 1.6b    Samsung (strap!=3) -> 1.6
//
// nvRev = NV2A PCI revision byte already read in LoadData() - passed in
//         to avoid a redundant PCI config read.
// ============================================================================

static void DetectBoardRevision(SysData& d, BYTE nvRev)
{
    BYTE b0 = 0, b1 = 0, b2 = 0;
    bool ok = SMBusRead(SMBADDR_PIC, 0x01, b0) &&
        SMBusRead(SMBADDR_PIC, 0x01, b1) &&
        SMBusRead(SMBADDR_PIC, 0x01, b2);

    d.boardRevPIC[0] = ok ? (char)b0 : '?';
    d.boardRevPIC[1] = ok ? (char)b1 : '?';
    d.boardRevPIC[2] = ok ? (char)b2 : '?';
    d.boardRevPIC[3] = '\0';   // properly null-terminated — adjacent boardRevFinal is NOT a backstop

    if (!ok) { StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), "UNKNOWN"); return; }

    // Dev kit variants (D01 / 01D / 1D0 etc.)
    if ((b0 == 'D' || b1 == 'D' || b2 == 'D') &&
        (b0 == '0' || b1 == '0' || b2 == '0') &&
        (b0 == '1' || b1 == '1' || b2 == '1'))
    {
        StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), "DEV KIT"); return;
    }

    // Debug / Alpha kit
    if ((b0 == 'D' && b1 == 'B' && b2 == 'G') || (b0 == 'B' && b1 == '1' && b2 == '1'))
    {
        StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), "DEBUG KIT"); return;
    }

    if (b0 == 'P' && b1 == '0' && b2 == '1')
    {
        StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), "1.0"); return;
    }

    if (b0 == 'P' && b1 == '0' && b2 == '5')
    {
        StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), "1.1"); return;
    }

    // 1.2 / 1.3 / 1.4 / 1.5  (all report P11 from PIC)
    if ((b0 == 'P' && b1 == '1' && b2 == '1') ||
        (b0 == '1' && b1 == 'P' && b2 == '1') ||
        (b0 == '1' && b1 == '1' && b2 == 'P'))
    {
        BYTE encId = 0;

        // Probe Conexant (0x8A) - present on 1.2 and 1.3.
        // NV2A PCI revision byte does NOT reliably distinguish 1.2 from 1.3 —
        // some 1.3 boards ship NV2A-A1 silicon.  Match PrometheOS: report 1.2/1.3.
        if (SMBusRead(SMBADDR_ENC_CNXT, 0x00, encId))
        {
            StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), "1.2/1.3");
            return;
        }

        // Probe Focus (0xD4) - present on 1.4 and 1.5
        if (SMBusRead(SMBADDR_ENC_FOCUS, 0x00, encId))
        {
            // FS454 (0x54) -> 1.4    FS455 (0x09) -> 1.5
            if (encId == 0x54) StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), "1.4");
            else if (encId == 0x09) StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), "1.5");
            else                    StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), "1.4/1.5");
            return;
        }

        StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), "1.2/1.3");
        return;
    }

    // 1.6 / 1.6b  (P2L)
    if (b0 == 'P' && b1 == '2' && b2 == 'L')
    {
        DWORD vendor = PciRead32(0, 1, 0, 0x00);
        if ((vendor & 0xFFFF) == 0x10DE)
        {
            volatile ULONG* emrs = (volatile ULONG*)0xFD101000;
            ULONG strap = (*emrs & 0x000C0000) >> 18;
            StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal),
                (strap == 3) ? "1.6b" : "1.6");
        }
        else
        {
            StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), "1.6");
        }
        return;
    }

    // Unrecognised PIC string - show raw bytes
    StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), d.boardRevPIC);
}

// ============================================================================
// Modchip detection  (ref: PrometheOS modchip::detectModchip + individual chip files)
// Uses direct port I/O reads at known modchip LPC signature addresses.
// Returns a static string — do not free.
// IMPORTANT: unknown result means TSOP/no modchip, NOT a probe failure.
//
// Probe order (most-unique signatures first to avoid aliasing):
//   Xecuter   0xF500 == 0xE1          (PrometheOS: active)
//   Modxo     0xDEAD == 0xAF          (PrometheOS: active)
//   Smartxx   0xF701 in {0xF1,F2,F8}  (PrometheOS: commented out, values confirmed)
//   Aladdin   0xF701 in {0x11,15,69}  (PrometheOS: active — read same port as above)
//   Xchanger  0x1912 != 0xFF          (PrometheOS: active)
//   Xtremium  0x00C  low nibble 1-10  (XTREMIUM_REGISTER_BANKING; distinct from Xenium)
//   Xenium    0x00EF low nibble 1-10  (XENIUM_REGISTER_BANKING; also Smartxx fallback,
//                                      but Smartxx caught above via 0xF701)
//
// Xenium/Xtremium/Smartxx were commented out of PrometheOS's detectModchip()
// because PrometheOS defaults to Xenium (it knows it launched from one).
// XbDiag cannot default — we use safe read-only bank-register probes instead.
// On an unmodded LPC bus, floating pulls return 0xFF; valid bank IDs are 0x01-0x0A.
// ============================================================================

static const char* DetectModchip()
{
    BYTE val = 0;

    // ---- Xecuter: port 0xF500 == 0xE1 ----
    __asm { mov dx, 0xF500 }
    __asm { in  al, dx     }
    __asm { mov val, al    }
    if (val == 0xE1) return "Xecuter";

    // ---- Modxo: port 0xDEAD == 0xAF ----
    __asm { mov dx, 0xDEAD }
    __asm { in  al, dx     }
    __asm { mov val, al    }
    if (val == 0xAF) return "Modxo";

    // ---- Smartxx + Aladdin share port 0xF701 — read once, branch on value ----
    // Smartxx values (from PrometheOS modchipSmartxx, commented-out probe):
    //   0xF1 = OGXbox Smartxx v1.0
    //   0xF2 = OGXbox Smartxx v2.0
    //   0xF8 = Smartxx OPX
    // Aladdin values: 0x11 (Lattice 1MB), 0x15 (Xilinx 1MB), 0x69 (Lattice 2MB)
    __asm { mov dx, 0xF701 }
    __asm { in  al, dx     }
    __asm { mov val, al    }
    if (val == 0xF1 || val == 0xF2 || val == 0xF8) return "Smartxx";
    if (val == 0x11 || val == 0x15)                return "Aladdin 1MB";
    if (val == 0x69)                               return "Aladdin 2MB";

    // ---- Xchanger: port 0x1912 != 0xFF ----
    __asm { mov dx, 0x1912 }
    __asm { in  al, dx     }
    __asm { mov val, al    }
    if (val != 0xFF) return "Xchanger";

    // ---- Xtremium: XTREMIUM_REGISTER_BANKING = 0x00C ----
    // Low nibble encodes the active bank; valid range 0x01-0x0A.
    // Xtremium uses a different banking port than Xenium (0x00C vs 0x00EF),
    // so we can distinguish them with read-only probes.
    __asm { mov dx, 0x000C }
    __asm { in  al, dx     }
    __asm { mov val, al    }
    {
        BYTE bank = val & 0x0F;
        if (bank >= 1 && bank <= 10) return "Xtremium";
    }

    // ---- Xenium: XENIUM_REGISTER_BANKING = 0x00EF ----
    // Same low-nibble bank encoding (1-10).
    // If we reach here Smartxx is already excluded (caught via 0xF701 above).
    __asm { mov dx, 0x00EF }
    __asm { in  al, dx     }
    __asm { mov val, al    }
    {
        BYTE bank = val & 0x0F;
        if (bank >= 1 && bank <= 10) return "Xenium";
    }

    // No known signature matched — TSOP or softmod boot
    return "None / TSOP";
}

// ============================================================================
// HD mod (HDMI adapter) detection  (ref: PrometheOS xboxConfig::getHdModString)
// Probes SMBus address 0x44 (sw-shifted 0x88) — Chimeric/HDMI adapters.
// On success reads version bytes from regs 0x57/0x58/0x59.
// ============================================================================

static void DetectHdMod(SysData& d)
{
    char tmp[16];
    char t[8];

    // ---- Chimeric / generic HDMI adapter (PrometheOS-compatible register protocol) ----
    // Probe: write then read reg 0 at sw-addr 0x88 (7-bit 0x44).
    // PrometheOS probes with a write first to wake the device.
    BYTE dummy = 0;
    bool probeOK = SMBusWrite(0x88, 0x00, 0x00) && SMBusRead(0x88, 0x00, dummy);

    BYTE busAddr = 0x88;
    if (!probeOK)
    {
        // Secondary address 0x86 (7-bit 0x43) used by some adapters
        probeOK = SMBusWrite(0x86, 0x00, 0x00) && SMBusRead(0x86, 0x00, dummy);
        if (probeOK) busAddr = 0x86;
    }

    if (probeOK)
    {
        // Read version regs 0x57/0x58/0x59
        BYTE v1 = 0, v2 = 0, v3 = 0;
        SMBusRead(busAddr, 0x57, v1);
        SMBusRead(busAddr, 0x58, v2);
        SMBusRead(busAddr, 0x59, v3);

        StrCopy(tmp, sizeof(tmp), "V");
        IntToStr(v1, t, sizeof(t)); StrCat2(tmp, sizeof(tmp), tmp, t);
        StrCat2(tmp, sizeof(tmp), tmp, ".");
        IntToStr(v2, t, sizeof(t)); StrCat2(tmp, sizeof(tmp), tmp, t);
        StrCat2(tmp, sizeof(tmp), tmp, ".");
        IntToStr(v3, t, sizeof(t)); StrCat2(tmp, sizeof(tmp), tmp, t);
        StrCopy(d.hdModVer, sizeof(d.hdModVer), tmp);
        return;
    }

    // ---- X-HD (Ryzee119 XboxHDMI) — command-based protocol at sw-addr 0xD2 (7-bit 0x69) ----
    // Same address as ICS clock generator; X-HD replaces it on the bus when installed.
    // Probe: read cmd 5 (READ_MODE) — expects 0x01 (bootloader) or 0x02 (application).
    // Version: cmds 1/2/3 return major/minor/patch bytes.
    BYTE mode = 0;
    if (SMBusRead(0xD2, 0x05, mode) && (mode == 0x01 || mode == 0x02))
    {
        BYTE v1 = 0, v2 = 0, v3 = 0;
        SMBusRead(0xD2, 0x01, v1);
        SMBusRead(0xD2, 0x02, v2);
        SMBusRead(0xD2, 0x03, v3);

        StrCopy(tmp, sizeof(tmp), "X-HD V");
        IntToStr(v1, t, sizeof(t)); StrCat2(tmp, sizeof(tmp), tmp, t);
        StrCat2(tmp, sizeof(tmp), tmp, ".");
        IntToStr(v2, t, sizeof(t)); StrCat2(tmp, sizeof(tmp), tmp, t);
        StrCat2(tmp, sizeof(tmp), tmp, ".");
        IntToStr(v3, t, sizeof(t)); StrCat2(tmp, sizeof(tmp), tmp, t);
        if (mode == 0x01)
            StrCat2(tmp, sizeof(tmp), tmp, " BL");   // sitting in bootloader
        StrCopy(d.hdModVer, sizeof(d.hdModVer), tmp);
        return;
    }

    StrCopy(d.hdModVer, sizeof(d.hdModVer), "Not Detected");
}

// ============================================================================
// Read all hardware data
// ============================================================================

static void ReadSysData()
{
    // Zero all fields so StrCat2 calls start from a clean slate on re-entry.
    ZeroMemory(&s_data, sizeof(s_data));
    SysData& d = s_data;

    // --- xemu / emulator detection ---
    // Hypervisor present bit: CPUID leaf 1, ECX bit 31.
    // Real Coppermine (family 6 model 8) never sets this bit.
    // KVM — which xemu uses — always sets it regardless of guest CPU.
    // Vendor string is NOT used: xemu passes through the host vendor
    // string, so an Intel host looks identical to real Xbox hardware.
    {
        DWORD ea, eb, ec, ed;
        DoCpuid(1, ea, eb, ec, ed);
        d.isXemu = (ec & 0x80000000UL) != 0;   // bit 31 = hypervisor present

        // Store raw CPUID leaf 1 EAX here unconditionally — both real hardware
        // and xemu paths use it on-screen; on xemu it reflects the host CPU.
        char eaxHex[10];
        IntToHex(ea, 8, eaxHex, sizeof(eaxHex));
        StrCat2(d.cpuLeaf1, sizeof(d.cpuLeaf1), "0x", eaxHex);
    }

    // --- CPU brand string (CPUID 0x80000002-4) ---
    // Guard: verify extended leaves up to 0x80000004 are supported first.
    {
        DWORD ea, eb, ec, ed;
        DoCpuid(0x80000000UL, ea, eb, ec, ed);
        bool brandSupported = (ea >= 0x80000004UL);

        if (brandSupported)
        {
            char* bp = d.cpuBrand;
            for (DWORD leaf = 0x80000002UL; leaf <= 0x80000004UL; ++leaf)
            {
                DoCpuid(leaf, ea, eb, ec, ed);
                DWORD r[4] = { ea, eb, ec, ed };
                for (int ri = 0; ri < 4; ++ri)
                {
                    bp[0] = (char)(r[ri] & 0xFF);
                    bp[1] = (char)((r[ri] >> 8) & 0xFF);
                    bp[2] = (char)((r[ri] >> 16) & 0xFF);
                    bp[3] = (char)((r[ri] >> 24) & 0xFF);
                    bp += 4;
                }
            }
            d.cpuBrand[47] = '\0';
            int start = 0;
            while (d.cpuBrand[start] == ' ') ++start;
            if (start > 0)
            {
                int i = 0;
                while (d.cpuBrand[start + i])
                {
                    d.cpuBrand[i] = d.cpuBrand[start + i]; ++i;
                }
                d.cpuBrand[i] = '\0';
            }
        }

        if (!brandSupported || d.cpuBrand[0] < 0x20 || d.cpuBrand[0] == (char)0xFF)
        {
            if (d.isXemu)
                StrCopy(d.cpuBrand, sizeof(d.cpuBrand), "Intel PIII 733 (xemu)");
            else
                StrCopy(d.cpuBrand, sizeof(d.cpuBrand), "Intel Pentium III");
        }
    }

    // --- CPU IC identification ---
    // Shown on screen as "CPU    :" and also written to export/text files.
    // Derived from CPUID leaf 1 family/model — reliable on all Xbox revisions
    // including Tualatin upgrades (family 6 model 0x0B).
    //   Model  8 (0x08) = Coppermine  (retail Xbox: 128KB L2)
    //   Model 11 (0x0B) = Tualatin    (upgrade: PIII-S 512KB or Celeron 256KB)
    //   Model  8 + "Celeron" in brand = Celeron Coppermine upgrade
    if (d.isXemu)
    {
        StrCopy(d.cpuIC, sizeof(d.cpuIC), d.cpuBrand);
        int icLen = 0;
        while (d.cpuIC[icLen]) ++icLen;
        if (icLen > 0 && icLen < (int)sizeof(d.cpuIC) - 1 && d.cpuIC[icLen - 1] != '*')
        {
            d.cpuIC[icLen] = '*'; d.cpuIC[icLen + 1] = '\0';
        }
    }
    else
    {
        DWORD ea1, eb1, ec1, ed1;
        DoCpuid(1, ea1, eb1, ec1, ed1);

        BYTE baseFamily = (BYTE)((ea1 >> 8) & 0x0F);
        BYTE baseModel = (BYTE)((ea1 >> 4) & 0x0F);
        BYTE extModel = (BYTE)((ea1 >> 16) & 0x0F);
        BYTE extFamily = (BYTE)((ea1 >> 20) & 0xFF);

        BYTE cpuFamily = baseFamily + (baseFamily == 0x0F ? extFamily : 0);
        BYTE cpuModel = baseModel + ((baseFamily == 6 || baseFamily == 0x0F)
            ? (extModel << 4) : 0);

        bool isCeleron = false;
        {
            const char* needle = "Celeron";
            const char* hay = d.cpuBrand;
            for (int hi = 0; hay[hi]; ++hi)
            {
                int ni = 0;
                while (needle[ni] && hay[hi + ni] == needle[ni]) ++ni;
                if (!needle[ni]) { isCeleron = true; break; }
            }
        }

        if (cpuFamily == 6 && cpuModel == 8)
        {
            if (isCeleron)
                StrCopy(d.cpuIC, sizeof(d.cpuIC), "Celeron (Coppermine)");
            else
                StrCopy(d.cpuIC, sizeof(d.cpuIC), "Coppermine-128");
        }
        else if (cpuFamily == 6 && cpuModel == 0x0B)
        {
            if (isCeleron)
                StrCopy(d.cpuIC, sizeof(d.cpuIC), "Tualatin-256 (Cel)");
            else
                StrCopy(d.cpuIC, sizeof(d.cpuIC), "Tualatin-512 (PIII-S)");
        }
        else if (cpuFamily == 6)
        {
            char mt[6];
            IntToHex(cpuModel, 2, mt, sizeof(mt));
            StrCopy(d.cpuIC, sizeof(d.cpuIC), "P6 model 0x");
            StrCat2(d.cpuIC, sizeof(d.cpuIC), d.cpuIC, mt);
        }
        else
        {
            char ft[4], mt[4];
            IntToHex(cpuFamily, 2, ft, sizeof(ft));
            IntToHex(cpuModel, 2, mt, sizeof(mt));
            StrCopy(d.cpuIC, sizeof(d.cpuIC), "Fam 0x");
            StrCat2(d.cpuIC, sizeof(d.cpuIC), d.cpuIC, ft);
            StrCat2(d.cpuIC, sizeof(d.cpuIC), d.cpuIC, " Mod 0x");
            StrCat2(d.cpuIC, sizeof(d.cpuIC), d.cpuIC, mt);
        }
    }

    // --- CPU speed (PLL: MCPX CPUMPLL + MSR 0x2A ratio) ---
    {
        d.cpuMHz = MeasureCpuMHz();
        // On xemu CPUMPLL/MSR may not reflect real hardware — flag with asterisk
        char t[12];
        IntToStr((int)d.cpuMHz, t, sizeof(t));
        StrCat2(d.cpuSpeedMHz, sizeof(d.cpuSpeedMHz), t, " MHz");
        if (d.isXemu)
            StrCat2(d.cpuSpeedMHz, sizeof(d.cpuSpeedMHz), d.cpuSpeedMHz, "*");
    }

    // --- Memory ---
    {
        MEMORYSTATUS ms; ms.dwLength = sizeof(ms);
        GlobalMemoryStatus(&ms);
        DWORD total = (DWORD)(ms.dwTotalPhys / (1024 * 1024));
        DWORD avail = (DWORD)(ms.dwAvailPhys / (1024 * 1024));
        char t[12];
        IntToStr((int)total, t, sizeof(t)); StrCat2(d.memTotal, sizeof(d.memTotal), t, " MB");
        IntToStr((int)avail, t, sizeof(t)); StrCat2(d.memAvail, sizeof(d.memAvail), t, " MB");
        StrCopy(d.memConfig, sizeof(d.memConfig),
            total >= 100 ? "4x32MB  dual rank" : "4x16MB  single rank");
    }

    // --- Chipset (NV2A bus 0 dev 0) ---
    BYTE pciRevByte = 0;
    {
        DWORD id = PciRead32(0, 0, 0, 0x00);
        DWORD rev = PciRead32(0, 0, 0, 0x08);
        WORD  dev = (WORD)(id >> 16);
        pciRevByte = (BYTE)(rev & 0xFF);
        char t[10];
        IntToHex(dev, 4, t, sizeof(t)); StrCat2(d.chipDevId, sizeof(d.chipDevId), "0x", t);
        IntToHex(pciRevByte, 2, t, sizeof(t)); StrCat2(d.chipRev, sizeof(d.chipRev), "0x", t);
    }

    // --- BIOS version ---
    // Reliable version detection requires parsing mod-chip-specific flash layouts
    // (Evox, X2, Cerbios, retail all differ). Not safe to scan flash blindly.
    // Report a placeholder - the VideoInfo module will surface encoder/AV info
    // which is more actionable than a BIOS string anyway.
    StrCopy(d.biosVer, sizeof(d.biosVer), "See VideoInfo");

    // --- Video encoder: probe Conexant (0x8A), Focus (0xD4), Xcalibur (0xE0) ---
    // PrometheOS reference: getEncoderString() tries 0x8A then 0xD4, else Xcalibur.
    BYTE encId = 0;
    {
        if (SMBusRead(SMBADDR_ENC_CNXT, 0x00, encId))
        {
            char t[4]; IntToHex(encId, 2, t, sizeof(t));
            StrCat2(d.encId, sizeof(d.encId), "0x", t);
            switch (encId)
            {
            case 0x76: StrCopy(d.encName, sizeof(d.encName), "Conexant CX25871"); break;
            default:
                StrCopy(d.encName, sizeof(d.encName), "Conexant 0x");
                StrCat2(d.encName, sizeof(d.encName), d.encName, t);
            }
        }
        else if (SMBusRead(SMBADDR_ENC_FOCUS, 0x00, encId))
        {
            char t[4]; IntToHex(encId, 2, t, sizeof(t));
            StrCat2(d.encId, sizeof(d.encId), "0x", t);
            switch (encId)
            {
            case 0x54: StrCopy(d.encName, sizeof(d.encName), "Focus FS454");  break;
            case 0x09: StrCopy(d.encName, sizeof(d.encName), "Focus FS455");  break;
            default:
                StrCopy(d.encName, sizeof(d.encName), "Focus 0x");
                StrCat2(d.encName, sizeof(d.encName), d.encName, t);
            }
        }
        else
        {
            // Xcalibur is integrated into Xcalibur ASIC — no separate I2C response
            StrCopy(d.encName, sizeof(d.encName), "Xcalibur (1.6)");
            StrCopy(d.encId, sizeof(d.encId), "N/A");
        }
    }

    // --- Board revision (PrometheOS getHardwareRevision method) ---
    DetectBoardRevision(d, pciRevByte);

    // --- AV pack (PIC SMBus 0x45 reg 0x04 bits [2:0]) ---
    {
        BYTE av = 0;
        if (SMBusRead(SMBADDR_PIC, 0x04, av))
        {
            switch (av & 0x07)
            {
            case 0: StrCopy(d.avPack, sizeof(d.avPack), "SCART");     break;
            case 1: StrCopy(d.avPack, sizeof(d.avPack), "HDTV");      break;
            case 2: StrCopy(d.avPack, sizeof(d.avPack), "VGA");       break;
            case 4: StrCopy(d.avPack, sizeof(d.avPack), "S-Video");   break;
            case 5: StrCopy(d.avPack, sizeof(d.avPack), "Composite"); break;
            case 6: StrCopy(d.avPack, sizeof(d.avPack), "None");      break;
            default:
            {
                char t[4]; IntToHex(av & 7, 1, t, sizeof(t));
                StrCopy(d.avPack, sizeof(d.avPack), "Unknown 0x");
                StrCat2(d.avPack, sizeof(d.avPack), d.avPack, t);
            }
            }
        }
        else StrCopy(d.avPack, sizeof(d.avPack), "PIC NAK");
    }

    // --- Thermal (cross-revision: ADM1032 on 1.0-1.5, PIC on 1.6) ---
    {
        d.rawTempAmbient = 0; d.rawTempCPU = 0;

        // Try ADM1032 first (hardware addr 0x4C, present on 1.0-1.5)
        BYTE admFrac = 0;
        bool useADM = SMBusRead(SMBADDR_ADM1032, 0x00, d.rawTempAmbient) &&
            SMBusRead(SMBADDR_ADM1032, 0x01, d.rawTempCPU);
        if (useADM) SMBusRead(SMBADDR_ADM1032, 0x10, admFrac);  // fractional byte (ref: PrometheOS)

        if (!useADM)
        {
            // 1.6: ADM1032 absent — fall back to PIC regs 0x09/0x0A.
            // PIC readings are noisier; take 3 samples and average.
            int accAmb = 0, accCPU = 0;
            int goodSamples = 0;
            for (int si = 0; si < 3; ++si)
            {
                BYTE pa = 0, pc = 0;
                if (SMBusRead(SMBADDR_PIC, 0x09, pc) && SMBusRead(SMBADDR_PIC, 0x0A, pa))
                {
                    accCPU += (int)pc;
                    accAmb += (int)pa;
                    ++goodSamples;
                }
            }
            if (goodSamples > 0)
            {
                d.rawTempAmbient = (BYTE)(accAmb / goodSamples);
                d.rawTempCPU = (BYTE)(accCPU / goodSamples);
                d.tempOK = true;
            }
            else
            {
                d.tempOK = false;
            }
        }
        else
        {
            d.tempOK = true;
        }

        char t[8];
        if (d.tempOK)
        {
            IntToStr((int)d.rawTempAmbient, t, sizeof(t));
            StrCat2(d.tempAmbient, sizeof(d.tempAmbient), t, " C");
            IntToStr((int)d.rawTempCPU, t, sizeof(t));
            StrCat2(d.tempCPU, sizeof(d.tempCPU), t, " C");
        }
        else
        {
            StrCopy(d.tempAmbient, sizeof(d.tempAmbient), "ERR");
            StrCopy(d.tempCPU, sizeof(d.tempCPU), "ERR");
        }
    }

    // --- EEPROM: serial + MAC via safe kernel path ---
    // ExQueryNonVolatileSetting(0xFFFF) reads all 256 bytes atomically.
    // Ref: PrometheOS XKEEPROM::ReadFromXBOX — the authoritative safe method.
    {
        BYTE  eepBuf[256];
        ULONG eeType = 0, eeLen = 0;
        LONG  eeOK = ExQueryNonVolatileSetting(0xFFFF, &eeType, eepBuf, 256, &eeLen);
        if (eeOK == 0 && eeLen >= 0x46)
        {
            // Serial: bytes 0x34-0x3F (12 ASCII chars)
            for (int i = 0; i < 12; ++i) d.serialNum[i] = (char)eepBuf[0x34 + i];
            d.serialNum[12] = '\0';

            // MAC: bytes 0x40-0x45 (ref: PrometheOS XKEEPROM EEPROMDATA struct)
            d.macOK = true;
            char* mp = d.macAddr;
            for (int i = 0; i < 6; ++i)
            {
                HexByte(eepBuf[0x40 + i], mp); mp += 2;
                if (i < 5) *mp++ = ':';
            }
            *mp = '\0';
        }
        else
        {
            StrCopy(d.serialNum, sizeof(d.serialNum), "EEPROM FAILED");
            d.macOK = false;
            StrCopy(d.macAddr, sizeof(d.macAddr), "EEPROM FAILED");
        }
    }

    // --- X-RTC expansion module (ref: PrometheOS getHasRtcExpansion) ---
    // DS1307/compatible at 7-bit addr 0x68 (8-bit 0xD0). Read reg 0 — ACK = present.
    {
        BYTE dummy = 0;
        d.rtcPresent = SMBusRead(0xD0, 0x00, dummy);
    }

    // --- I2C display detection ---
    // Common addresses: PCF8574 I2C backpack 0x27 (8-bit 0x4E)
    //                   SSD1306/SSD1309 OLED  0x3C (8-bit 0x78)
    //                   SSD1306 alt address   0x3D (8-bit 0x7A)
    {
        BYTE dummy = 0;
        d.dispPresent = false;
        d.dispAddr[0] = '\0';
        if (SMBusRead(0x4E, 0x00, dummy))
        {
            d.dispPresent = true;
            StrCopy(d.dispAddr, sizeof(d.dispAddr), "0x27");
        }
        else if (SMBusRead(0x78, 0x00, dummy))
        {
            d.dispPresent = true;
            StrCopy(d.dispAddr, sizeof(d.dispAddr), "0x3C");
        }
        else if (SMBusRead(0x7A, 0x00, dummy))
        {
            d.dispPresent = true;
            StrCopy(d.dispAddr, sizeof(d.dispAddr), "0x3D");
        }
    }

    // --- Modchip detection (ref: PrometheOS modchip::detectModchip) ---
    StrCopy(d.modchipName, sizeof(d.modchipName), DetectModchip());

    // --- HD mod / HDMI adapter detection (ref: PrometheOS getHdModString) ---
    DetectHdMod(d);
    {
        WORD buf[256];
        d.hddPresent = AtaIdentify(buf);
        if (d.hddPresent)
        {
            AtaSwapStr(&buf[27], 20, d.hddModel, sizeof(d.hddModel));
            AtaSwapStr(&buf[10], 10, d.hddSerial, sizeof(d.hddSerial));
            // Prefer LBA48 (words 100-103) if supported (word 83 bit 10).
            // Needed for modded Xboxes with large HDDs (>137GB).
            // Fall back to LBA28 (words 60-61) for standard drives.
            DWORD sectorLo, sectorHi;
            bool lba48 = !!(buf[83] & (1 << 10));
            if (lba48)
            {
                sectorLo = ((DWORD)buf[101] << 16) | (DWORD)buf[100];
                sectorHi = ((DWORD)buf[103] << 16) | (DWORD)buf[102];
            }
            else
            {
                sectorLo = ((DWORD)buf[61] << 16) | (DWORD)buf[60];
                sectorHi = 0;
            }
            // gb = sectorHi * 2048 + sectorLo / 2097152
            DWORD gb = (sectorHi > 0 ? sectorHi * 2048UL : 0UL)
                + sectorLo / (2UL * 1024UL * 1024UL);
            char t[12]; IntToStr((int)gb, t, sizeof(t));
            StrCat2(d.hddSizeGB, sizeof(d.hddSizeGB), t, " GB");
            // UDMA detection (ATA/ATAPI-7 T13 spec):
            //   Word 53 bit 2: must be set for word 88 to be valid.
            //   Word 88 bits 6:0  = supported UDMA modes (bit N = mode N supported).
            //   Word 88 bits 14:8 = active   UDMA modes (bit 8+N = mode N active).
            //
            // Xbox MCPX behavior:
            //   The MCPX/nForce IDE controller programs DMA mode in its own
            //   PCI timing registers without issuing a software SET FEATURES
            //   command to the drive.  As a result, the "active" bits in word 88
            //   (bits 14:8) are often zero even though the drive IS running UDMA.
            //   Treat a word53-valid, zero-active, non-zero-supported result as
            //   "MCPX-configured UDMA" — report the highest supported mode without
            //   the asterisk, since the hardware IS using it; asterisk is reserved
            //   for the case where word 53 bit 2 is NOT set (word 88 unreliable).
            {
                bool w88valid = !!(buf[53] & (1 << 2));  // word 53 bit 2

                BYTE active = (BYTE)(buf[88] >> 8) & 0x7F;  // bits 14:8 -> 6:0
                BYTE supported = (BYTE)(buf[88] & 0xFF) & 0x7F;  // bits  6:0

                int  mode = -1;
                bool needAster = false;   // asterisk = "word 88 validity uncertain"

                if (w88valid && (active || supported))
                {
                    // Prefer confirmed-active bits; fall back to supported.
                    // On MCPX consoles active is usually 0; use supported cleanly.
                    BYTE use = active ? active : supported;
                    for (int i = 6; i >= 0; --i)
                        if (use & (1 << i)) { mode = i; break; }

                    // Only mark with asterisk if we fell back AND word 53 says
                    // word 88 is not valid (should never reach here, but guard anyway).
                    needAster = false;
                }
                else if (!w88valid && (active || supported))
                {
                    // Word 53 bit 2 clear — word 88 data is not ATA-spec-guaranteed.
                    // Still try to use it but flag with asterisk.
                    BYTE use = active ? active : supported;
                    for (int i = 6; i >= 0; --i)
                        if (use & (1 << i)) { mode = i; break; }
                    needAster = true;
                }

                if (mode >= 0)
                {
                    char t2[4]; IntToStr(mode, t2, sizeof(t2));
                    StrCopy(d.hddUDMA, sizeof(d.hddUDMA), "UDMA");
                    StrCat2(d.hddUDMA, sizeof(d.hddUDMA), d.hddUDMA, t2);
                    if (needAster) StrCat2(d.hddUDMA, sizeof(d.hddUDMA), d.hddUDMA, "?");
                }
                else
                {
                    // Word 88 was zero or unusable — fall back to MWDMA (word 63).
                    // Word 63 bits 10:8 = active MWDMA, bits 2:0 = supported MWDMA.
                    BYTE mwActive = (BYTE)(buf[63] >> 8) & 0x07;
                    BYTE mwSupp = (BYTE)(buf[63] & 0xFF) & 0x07;
                    int  mwMode = -1;
                    bool mwFall = false;

                    for (int i = 2; i >= 0; --i)
                        if (mwActive & (1 << i)) { mwMode = i; break; }
                    if (mwMode < 0)
                        for (int i = 2; i >= 0; --i)
                            if (mwSupp & (1 << i)) { mwMode = i; mwFall = true; break; }

                    if (mwMode >= 0)
                    {
                        char t2[4]; IntToStr(mwMode, t2, sizeof(t2));
                        StrCopy(d.hddUDMA, sizeof(d.hddUDMA), "MWDMA");
                        StrCat2(d.hddUDMA, sizeof(d.hddUDMA), d.hddUDMA, t2);
                        if (mwFall) StrCat2(d.hddUDMA, sizeof(d.hddUDMA), d.hddUDMA, "?");
                    }
                    else StrCopy(d.hddUDMA, sizeof(d.hddUDMA), "PIO");
                }
            }
        }
        else
        {
            StrCopy(d.hddModel, sizeof(d.hddModel), "Not detected");
            StrCopy(d.hddSerial, sizeof(d.hddSerial), "N/A");
            StrCopy(d.hddSizeGB, sizeof(d.hddSizeGB), "N/A");
            StrCopy(d.hddUDMA, sizeof(d.hddUDMA), "N/A");
        }
    }



    // MAC is now read in the EEPROM block above (ExQueryNonVolatileSetting).

    // --- Local IP address ---
    // XNet was started in SysInfo_OnEnter() before the loading frame rendered,
    // giving DHCP extra time to complete.  Poll here until resolved or timeout.
    {
        d.ipOK = false;
        StrCopy(d.ipAddr, sizeof(d.ipAddr), "No Link");

        // Poll XNetGetTitleXnAddr — up to 5 seconds (50 x 100ms).
        // We're behind the loading screen so the user sees no delay.
        XNADDR xna;
        ZeroMemory(&xna, sizeof(xna));
        DWORD st = XNetGetTitleXnAddr(&xna);
        for (int pi = 0; pi < 50 && st == XNET_GET_XNADDR_PENDING; ++pi)
        {
            Sleep(100);
            st = XNetGetTitleXnAddr(&xna);
        }

        if (!(st & XNET_GET_XNADDR_NONE) && xna.ina.s_addr != 0)
        {
            BYTE* b = (BYTE*)&xna.ina.s_addr;
            char* p = d.ipAddr;
            char oct[6];
            for (int oi = 0; oi < 4; ++oi)
            {
                IntToStr((int)b[oi], oct, sizeof(oct));
                const char* sp = oct;
                while (*sp) *p++ = *sp++;
                if (oi < 3) *p++ = '.';
            }
            *p = '\0';
            d.ipOK = true;
        }
    }

    // --- GPU speed (NV2A PRAMDAC PLL) ---
    {
        d.gpuMHz = ReadGpuMHz();
        char t[12]; IntToStr((int)d.gpuMHz, t, sizeof(t));
        StrCat2(d.gpuSpeedMHz, sizeof(d.gpuSpeedMHz), t, " MHz");
    }

    d.exportDone = false;
    d.exportOK = false;

    // --- Flash chip (JEDEC autoselect, guarded by modchip + rev 1.6 checks) ---
    DetectFlashChip(s_flashInfo);
}

// ============================================================================
// Export
// ============================================================================

static void WriteLine(HANDLE hf, const char* label, const char* value)
{
    char line[128]; DWORD w;
    StrCopy(line, sizeof(line), label);
    StrCat2(line, sizeof(line), line, value);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
}

// ============================================================================
// BIOS dump
// ============================================================================
//
// The Xbox flash ROM is hardware-decoded by the MCPX chip across the top 16MB
// of physical address space (0xFF000000 - 0xFFFFFFFF). The ROM is aliased
// throughout this range. Conventional base addresses used by software:
//   0xFFFC0000 — last 256KB alias (retail / 256KB chip)
//   0xFFF00000 — last 1MB alias   (mod chip / 1MB chip)
//
// On Xbox there is no IOMMU — physical == virtual for ROM/device regions, so
// direct pointer dereference is safe for mapped ROM space.
//
// DANGER ZONE: 0xFFFFFE00 - 0xFFFFFFFF (top 512 bytes).
// This is the hidden MCPX ROM on revisions 1.1-1.6. The MCPX hides itself
// during boot before the kernel hands off to the XBE. Accessing this range
// from a running XBE WILL freeze the system. Hard cap all reads at 0xFFFFFDFF.
//
// Size detection: compare 16 bytes at 0xFFF00000 vs 0xFFFC0000.
// On a 256KB chip the ROM tiles four times — the banks are identical.
// On a genuine 1MB mod chip the banks differ.
//
// Output: D:\bios.bin — XBE launch directory.
// ============================================================================
// MmMapIoSpace / MmUnmapIoSpace -- kernel exports at ordinals 177 / 183.
// Declared the same way as HalReadSMBusValue -- plain extern "C", no dllimport.
// PhysicalAddress is ULONG on Xbox (32-bit, confirmed by @12 stack frame).
extern "C"
{
    PVOID __stdcall MmMapIoSpace(
        ULONG PhysicalAddress,
        ULONG NumberOfBytes,
        ULONG Protect);

    VOID __stdcall MmUnmapIoSpace(
        PVOID BaseAddress,
        ULONG NumberOfBytes);

    // IoCreateSymbolicLink — mounts a drive letter before first use.
    // Returns 0xC0000035 if already mounted, which is fine — just ignore it.
    typedef struct { USHORT len; USHORT maxLen; char* buf; } XBIOS_STRING;
    LONG WINAPI IoCreateSymbolicLink(XBIOS_STRING* symLink, XBIOS_STRING* target);
}

static void DumpBios()
{
    SysData& d = s_data;

    static const ULONG PHYS_256KB = 0xFFFC0000UL;
    static const ULONG PHYS_1MB = 0xFFF00000UL;

    // Full aligned sizes — what a correct image file should be
    static const ULONG FULL_256KB = 0x40000UL;   // 262144 bytes
    static const ULONG FULL_1MB = 0x100000UL;  // 1048576 bytes

    // Hard ceiling: never map right up to 0xFFFFFFFF.
    // MmMapIoSpace(0xFFFC0000, 0x40000) would compute end = 0x100000000,
    // a 32-bit overflow to zero — the kernel maps the wrong range and the
    // system freezes. Cap at 0xFFFFFDFF (safe on all revisions).
    //
    // On rev 1.1-1.6 this also avoids the MCPX ROM shadow (0xFFFFFE00-0xFFFFFFFF)
    // which cannot be read from a running XBE.
    //
    // We pad the output file to the correct aligned size with 0xFF (erased flash
    // state) so the dump is a valid 256KB or 1MB image on all boards.
    static const ULONG SAFE_TOP = 0xFFFFFE00UL;
    static const ULONG SIZE_256KB_SAFE = SAFE_TOP - PHYS_256KB;  // 0x3FE00 = 261632
    static const ULONG SIZE_1MB_SAFE = SAFE_TOP - PHYS_1MB;    // 0xFFE00 = 1048064

    // Protect value 4 = MmNonCached -- required for ROM / device regions.
    static const ULONG MM_NONCACHED = 4;

    // ---- Size detection: probe 16 bytes at each base ----
    // Both addresses are well below SAFE_TOP.
    bool is1MB = false;
    {
        BYTE* v256 = (BYTE*)MmMapIoSpace(PHYS_256KB, 16, MM_NONCACHED);
        BYTE* v1M = (BYTE*)MmMapIoSpace(PHYS_1MB, 16, MM_NONCACHED);

        if (!v256 || !v1M)
        {
            if (v256) MmUnmapIoSpace(v256, 16);
            if (v1M)  MmUnmapIoSpace(v1M, 16);
            d.biosDumpError = 1; d.biosDumpDone = true; d.biosDumpOK = false; return;
        }

        for (int bi = 0; bi < 16; ++bi)
        {
            if (v1M[bi] != v256[bi]) { is1MB = true; break; }
        }

        MmUnmapIoSpace(v256, 16);
        MmUnmapIoSpace(v1M, 16);
    }

    ULONG dumpPhys = is1MB ? PHYS_1MB : PHYS_256KB;
    ULONG readSize = is1MB ? SIZE_1MB_SAFE : SIZE_256KB_SAFE;
    ULONG fullSize = is1MB ? FULL_1MB : FULL_256KB;
    ULONG padSize = fullSize - readSize;  // always 512 bytes

    // ---- Open output file BEFORE mapping ROM (avoids heap corruption risk) ----
    HANDLE hf = CreateFileA("\\bios.bin", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        d.biosDumpLastErr = GetLastError();
        d.biosDumpError = 2; d.biosDumpDone = true; d.biosDumpOK = false; return;
    }

    // ---- Map the full dump region ----
    BYTE* mapped = (BYTE*)MmMapIoSpace(dumpPhys, readSize, MM_NONCACHED);
    if (!mapped)
    {
        CloseHandle(hf);
        d.biosDumpError = 1; d.biosDumpDone = true; d.biosDumpOK = false; return;
    }

    // ---- Write in 4KB pages ----
    const ULONG PAGE = 4096;
    DWORD written = 0;
    bool  ok = true;
    for (ULONG offset = 0; offset < readSize && ok; offset += PAGE)
    {
        ULONG chunk = (readSize - offset < PAGE) ? (readSize - offset) : PAGE;
        DWORD w = 0;
        if (!WriteFile(hf, mapped + offset, chunk, &w, NULL) || w != chunk)
        {
            ok = false; d.biosDumpError = 3;
        }
        else
            written += w;
    }

    MmUnmapIoSpace(mapped, readSize);

    // ---- Pad to aligned size on rev 1.1+ (MCPX shadow region = 0xFF fill) ----
    // The last 512 bytes of flash are overlaid by the MCPX ROM at runtime and
    // cannot be read by memory-mapped access on rev 1.1-1.6. Fill with 0xFF
    // (erased flash state) so the output file is always a valid 256KB or 1MB
    // image that flashers and emulators will accept without size complaints.
    if (ok && padSize > 0)
    {
        BYTE padBuf[512];
        for (ULONG i = 0; i < padSize && i < sizeof(padBuf); ++i) padBuf[i] = 0xFF;
        DWORD w = 0;
        ULONG toWrite = (padSize <= sizeof(padBuf)) ? padSize : sizeof(padBuf);
        if (!WriteFile(hf, padBuf, toWrite, &w, NULL) || w != toWrite)
        {
            ok = false; d.biosDumpError = 3;
        }
        else
            written += w;
    }

    CloseHandle(hf);

    d.biosDumpSize = written;
    d.biosDumpDone = true;
    d.biosDumpOK = ok;
}

// ============================================================================
// Flash chip detection (JEDEC autoselect)
// ============================================================================
//
// Xbox TSOP is mapped by MCPX at physical 0xFF000000 (same window as BIOS).
// JEDEC autoselect sequence: AA→5555, 55→2AAA, 90→5555, read [0]=mfr [1]=dev,
// then F0→[0] to exit autoselect mode.
//
// WE# must be bridged (TSOP mod) for writes to reach the chip.
// If WE# is not bridged, write cycles are ignored — autoselect never activates
// and reads return normal ROM data (not 0xDA/0x0B etc.).  Only chips whose
// JEDEC IDs appear in the known-ID table are marked flashable.
//
// Guards checked before any I/O:
//   - Modchip active  → modchip owns the LPC bus; TSOP not reachable
//   - Rev 1.6/1.6b   → Xcalibur GPU has no TSOP socket
//
static void DetectFlashChip(FlashInfo& fi)
{
    // zero-init
    fi.probed = false;
    fi.found = false;
    fi.flashable = false;
    fi.modchipGuard = false;
    fi.rev16Guard = false;
    fi.mfrId = 0xFF;
    fi.devId = 0xFF;
    StrCopy(fi.chipName, sizeof(fi.chipName), "Unknown");
    StrCopy(fi.sizeStr, sizeof(fi.sizeStr), "");
    StrCopy(fi.mfrHex, sizeof(fi.mfrHex), "0x??");
    StrCopy(fi.devHex, sizeof(fi.devHex), "0x??");

    // Guard 1: modchip active — LPC bus intercepted, TSOP unreachable
    // modchipName == "None / TSOP" is the only no-modchip value
    if (s_data.modchipName[0] != 'N')
    {
        fi.modchipGuard = true;
        return;
    }

    // Guard 2: rev 1.6 / 1.6b — Xcalibur chip, no TSOP socket
    {
        const char* rev = s_data.boardRevFinal;
        if (rev[0] == '1' && rev[1] == '.' && rev[2] == '6')
        {
            fi.rev16Guard = true;
            return;
        }
    }

    fi.probed = true;

    // Map 64KB at 0xFF000000 — covers offsets 0x0000–0xFFFF (incl. 0x5555/0x2AAA)
    static const ULONG TSOP_PHYS = 0xFF000000UL;
    static const ULONG MAP_SIZE = 0x10000UL;
    static const ULONG MM_NONCACHED = 4;

    BYTE* base = (BYTE*)MmMapIoSpace(TSOP_PHYS, MAP_SIZE, MM_NONCACHED);
    if (!base)
        return;

    // JEDEC autoselect entry
    base[0x5555] = 0xAA;
    base[0x2AAA] = 0x55;
    base[0x5555] = 0x90;

    BYTE mfr = base[0];
    BYTE dev = base[1];

    // Exit autoselect
    base[0] = 0xF0;

    MmUnmapIoSpace(base, MAP_SIZE);

    fi.mfrId = mfr;
    fi.devId = dev;

    // Build hex strings (no sprintf — manual nibble print)
    static const char s_hexNib[] = "0123456789ABCDEF";
    fi.mfrHex[0] = '0'; fi.mfrHex[1] = 'x';
    fi.mfrHex[2] = s_hexNib[mfr >> 4]; fi.mfrHex[3] = s_hexNib[mfr & 0xF]; fi.mfrHex[4] = '\0';
    fi.devHex[0] = '0'; fi.devHex[1] = 'x';
    fi.devHex[2] = s_hexNib[dev >> 4]; fi.devHex[3] = s_hexNib[dev & 0xF]; fi.devHex[4] = '\0';

    // Known Xbox TSOP chips (WE# must be bridged for JEDEC to respond)
    struct ChipEntry { BYTE mfr; BYTE dev; const char* name; const char* size; };
    static const ChipEntry s_chips[] = {
        // 256KB (1.2–1.5) — dominant population
        { 0xDA, 0x0B, "Winbond W49F002U",      "256KB" },
        { 0xDA, 0x8C, "Winbond W49F020",        "256KB" },
        { 0xC2, 0x36, "Macronix MX29F022NTPC",  "256KB" },
        { 0x20, 0xB0, "ST M29F020",              "256KB" },
        // 1MB (1.0–1.1 only)
        { 0x01, 0xD5, "AMD Am29F080B",           "1MB"   },
        { 0x04, 0xD5, "Fujitsu MBM29F080A",      "1MB"   },
        { 0xAD, 0xD5, "Hynix HY29F080",          "1MB"   },
        { 0x20, 0xF1, "ST M29F080A",              "1MB"   },
        { 0x89, 0xA6, "Sharp LH28F008SCT",        "1MB"   },
        { 0x00, 0x00, NULL, NULL }   // sentinel
    };

    for (int i = 0; s_chips[i].name != NULL; ++i)
    {
        if (s_chips[i].mfr == mfr && s_chips[i].dev == dev)
        {
            fi.found = true;
            fi.flashable = true;
            StrCopy(fi.chipName, sizeof(fi.chipName), s_chips[i].name);
            StrCopy(fi.sizeStr, sizeof(fi.sizeStr), s_chips[i].size);
            return;
        }
    }

    // IDs returned but not in table
    if (mfr == 0xFF && dev == 0xFF)
    {
        // Autoselect did not activate: WE# not bridged, or no TSOP populated
        StrCopy(fi.chipName, sizeof(fi.chipName), "Not found");
        return;
    }

    // Unrecognized non-FF IDs — chip present but unknown command set
    StrCopy(fi.chipName, sizeof(fi.chipName), "Unknown chip");
    fi.found = true;
}

static void ExportSysInfo()
{
    HANDLE hf = CreateFileA("D:\\sysinfo.txt", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        s_data.exportDone = true; s_data.exportOK = false; return;
    }

    const SysData& d = s_data;
    DWORD w;
    const char* hdr = "XbDiag System Info\r\n==================\r\n";
    WriteFile(hf, hdr, StrLen(hdr), &w, NULL);
    WriteLine(hf, "CPU Brand:     ", d.cpuBrand);
    WriteLine(hf, "CPU IC:        ", d.cpuIC);
    WriteLine(hf, "CPU Speed:     ", d.cpuSpeedMHz);
    WriteLine(hf, "GPU Speed:     ", d.gpuSpeedMHz);
    WriteLine(hf, "Mem Total:     ", d.memTotal);
    WriteLine(hf, "Mem Avail:     ", d.memAvail);
    WriteLine(hf, "Mem Config:    ", d.memConfig);
    WriteLine(hf, "Chipset Dev:   ", d.chipDevId);
    WriteLine(hf, "Chipset Rev:   ", d.chipRev);
    WriteLine(hf, "Board Rev PIC: ", d.boardRevPIC);
    WriteLine(hf, "Board Rev:     ", d.boardRevFinal);
    WriteLine(hf, "Serial Num:    ", d.serialNum);
    WriteLine(hf, "X-RTC:         ", d.rtcPresent ? "Present" : "Not detected");
    if (d.dispPresent)
    {
        char dispLine[24];
        StrCat2(dispLine, sizeof(dispLine), "Display @ ", d.dispAddr);
        WriteLine(hf, "LCD Display:   ", dispLine);
    }
    else
    {
        WriteLine(hf, "LCD Display:   ", "Not detected");
    }
    WriteLine(hf, "Modchip:       ", d.modchipName);
    WriteLine(hf, "HD Mod:        ", d.hdModVer);
    WriteLine(hf, "BIOS:          ", d.biosVer);
    WriteLine(hf, "Encoder:       ", d.encName);
    WriteLine(hf, "AV Pack:       ", d.avPack);
    WriteLine(hf, "Temp Ambient:  ", d.tempAmbient);
    WriteLine(hf, "Temp CPU:      ", d.tempCPU);
    WriteLine(hf, "HDD Model:     ", d.hddModel);
    WriteLine(hf, "HDD Serial:    ", d.hddSerial);
    WriteLine(hf, "HDD Size:      ", d.hddSizeGB);
    WriteLine(hf, "HDD UDMA:      ", d.hddUDMA);
    WriteLine(hf, "MAC Address:   ", d.macAddr);
    WriteLine(hf, "IP Address:    ", d.ipAddr);
    CloseHandle(hf);
    s_data.exportDone = true;
    s_data.exportOK = true;
}

// ============================================================================
// OnEnter
// ============================================================================

void SysInfo_OnEnter()
{
    s_prevBtns = 0;
    s_dataLoaded = false;
    s_flashPopupOpen = false;
    s_data.exportDone = false;
    s_data.exportOK = false;
    s_data.biosDumpDone = false;
    s_data.biosDumpOK = false;
    s_data.biosDumpSize = 0;
    s_data.biosDumpError = 0;

    // Mount HDD partitions — kernel only auto-mounts D: (title dir).
    // C/E/F/G need explicit symlinks; returns 0xC0000035 if already mounted
    // which is fine — always safe to call.
    {
        static const struct { const char* lnk; const char* dev; } k[] =
        {
            { "\\??\\C:", "\\Device\\Harddisk0\\Partition2" },
            { "\\??\\E:", "\\Device\\Harddisk0\\Partition1" },
            { "\\??\\F:", "\\Device\\Harddisk0\\Partition6" },
            { "\\??\\G:", "\\Device\\Harddisk0\\Partition7" },
        };
        char lnkBuf[8];
        for (int i = 0; i < 4; ++i)
        {
            const char* l = k[i].lnk;
            int ll = 0; while (l[ll]) ll++;
            for (int j = 0; j < ll; ++j) lnkBuf[j] = l[j];
            const char* dev = k[i].dev;
            int dl = 0; while (dev[dl]) dl++;
            XBIOS_STRING sLnk = { (USHORT)ll, (USHORT)(ll + 1), lnkBuf };
            XBIOS_STRING sDev = { (USHORT)dl, (USHORT)(dl + 1), (char*)dev };
            IoCreateSymbolicLink(&sLnk, &sDev);
        }
    }

    // Start the network stack here, before the loading frame renders.
    // DHCP needs time to complete; starting early and polling later in
    // ReadSysData gives it the best chance.  XNetStartup is ref-counted
    // so this is safe whether or not the dashboard left the stack up.
    XNetStartupParams xnsp;
    ZeroMemory(&xnsp, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    XNetStartup(&xnsp);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

// ============================================================================
// Render helpers
// ============================================================================
// Flash chip popup card
// ============================================================================

static void DrawFlashPopup()
{
    const FlashInfo& fi = s_flashInfo;

    // Card dimensions (design space 640x480)
    const float CW = 360.f;
    const float CH = 142.f;
    const float CX = (SW - CW) * 0.5f;
    const float CY = (480.f - CH) * 0.5f;
    const float CX1 = CX + CW;
    const float CY1 = CY + CH;
    const float PAD = 12.f;
    const float LH = LINE_H;

    // Shadow
    FillRect(CX + 4.f, CY + 4.f, CX1 + 4.f, CY1 + 4.f,
        D3DCOLOR_ARGB(140, 0, 0, 0));

    // Background
    FillRectGrad(CX, CY, CX1, CY1,
        D3DCOLOR_XRGB(14, 22, 52),
        D3DCOLOR_XRGB(8, 14, 36));

    // Border
    HLine(CY, CX, CX1, COL_BORDER);
    HLine(CY1, CX, CX1, COL_BORDER);
    VLine(CX, CY, CY1, COL_BORDER);
    VLine(CX1, CY, CY1, COL_BORDER);

    // Title bar
    FillRect(CX + 1.f, CY + 1.f, CX1 - 1.f, CY + LH + 6.f,
        D3DCOLOR_XRGB(20, 40, 90));
    HLine(CY + LH + 6.f, CX + 1.f, CX1 - 1.f, COL_BORDER);
    DrawText(CX + PAD, CY + 3.f, "FLASH CHIP INFO", 1.2f, COL_YELLOW);

    float y = CY + LH + 10.f;
    const float VX = CX + PAD + 76.f;   // value column

    if (fi.modchipGuard)
    {
        DrawText(CX + PAD, y, "CHIP   :", 1.2f, COL_GRAY);
        DrawText(VX, y, "TSOP unavailable  (modchip active)", 1.2f, COL_DIM);
        y += LH;
    }
    else if (fi.rev16Guard)
    {
        DrawText(CX + PAD, y, "CHIP   :", 1.2f, COL_GRAY);
        DrawText(VX, y, "No TSOP  (rev 1.6 - Xcalibur only)", 1.2f, COL_DIM);
        y += LH;
    }
    else if (!fi.probed)
    {
        DrawText(CX + PAD, y, "CHIP   :", 1.2f, COL_GRAY);
        DrawText(VX, y, "Probe failed  (map error)", 1.2f, COL_DIM);
        y += LH;
    }
    else
    {
        // CHIP name
        DrawText(CX + PAD, y, "CHIP   :", 1.2f, COL_GRAY);
        DrawText(VX, y, fi.chipName, 1.2f,
            fi.found ? COL_CYAN : COL_DIM);
        y += LH;

        // MFR / DEV / SIZE on one line
        if (fi.mfrId != 0xFF || fi.devId != 0xFF)
        {
            // "MFR: 0xDA   DEV: 0x0B   SIZE: 256KB"
            static char s_ids[48];
            StrCopy(s_ids, sizeof(s_ids), "MFR: ");
            StrCat2(s_ids, sizeof(s_ids), s_ids, fi.mfrHex);
            StrCat2(s_ids, sizeof(s_ids), s_ids, "   DEV: ");
            StrCat2(s_ids, sizeof(s_ids), s_ids, fi.devHex);
            if (fi.sizeStr[0])
            {
                StrCat2(s_ids, sizeof(s_ids), s_ids, "   SIZE: ");
                StrCat2(s_ids, sizeof(s_ids), s_ids, fi.sizeStr);
            }
            DrawText(CX + PAD, y, "       :", 1.2f, COL_GRAY);
            DrawText(VX, y, s_ids, 1.1f, COL_WHITE);
            y += LH;
        }

        // Flashable status line
        y += 4.f;
        if (fi.flashable)
        {
            DrawText(CX + PAD, y, "Flashable  (WE# bridged)", 1.2f, COL_GREEN);
        }
        else if (fi.found)
        {
            DrawText(CX + PAD, y, "Flashable  (check WE# bridge)", 1.2f, COL_YELLOW);
        }
        else
        {
            DrawText(CX + PAD, y, "Not flashable  (WE# not bridged)", 1.2f, COL_DIM);
        }
        y += LH;
    }

    // Close hint
    DrawText(CX + PAD, CY1 - LH - 2.f, "[B] Close", 1.2f, COL_YELLOW);
}

// ============================================================================

static void DrawSection(float x, float y, float ruleW, const char* title)
{
    DrawText(x, y, title, 1.3f, COL_YELLOW);
    HLine(y + LINE_H, x, x + ruleW, COL_BORDER);
}

static void DrawRow(float lx, float y, float vx,
    const char* label, const char* val, DWORD vc)
{
    DrawText(lx, y, label, 1.2f, COL_GRAY);
    DrawText(vx, y, val, 1.2f, vc);
}

// ============================================================================
// Render
// ============================================================================

static void Render(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    const char* hint;
    if (s_flashPopupOpen)
        hint = "[B] Close flash info";
    else if (s_data.biosDumpDone)
        if (s_data.biosDumpOK)
            hint = "[A] Export  [X] Flash  [Y] BIOS dumped OK  [B] Back";
        else if (s_data.biosDumpError == 1)
            hint = "[A] Export  [X] Flash  [Y] FAIL: map err  [B] Back";
        else if (s_data.biosDumpError == 2)
        {
            static char s_fileErrHint[64];
            StrCopy(s_fileErrHint, sizeof(s_fileErrHint), "[A] Export  [X] Flash  [Y] FAIL:file ");
            char tmp[12]; IntToStr((int)s_data.biosDumpLastErr, tmp, sizeof(tmp));
            StrCat2(s_fileErrHint, sizeof(s_fileErrHint), s_fileErrHint, tmp);
            StrCat2(s_fileErrHint, sizeof(s_fileErrHint), s_fileErrHint, "  [B] Back");
            hint = s_fileErrHint;
        }
        else if (s_data.biosDumpError == 3)
            hint = "[A] Export  [X] Flash  [Y] FAIL: write err  [B] Back";
        else
            hint = "[A] Export  [X] Flash  [Y] BIOS dump FAIL  [B] Back";
    else if (s_data.exportDone)
        hint = s_data.exportOK ? "[A] Exported OK  [X] Flash  [Y] Dump BIOS  [B] Back"
        : "[A] Export fail  [X] Flash  [Y] Dump BIOS  [B] Back";
    else
        hint = "[A] Export    [X] Flash    [Y] Dump BIOS    [B] Back";

    DrawPageChrome(logo, "SYSTEM INFO", hint);

    if (!s_dataLoaded)
    {
        DrawText(LM, CONTENT_Y + 20.f, "Reading hardware...", 1.4f, COL_YELLOW);
        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
        return;
    }

    const SysData& d = s_data;

    // ---- Layout constants (design space) ----
    const float C1 = LM;
    const float C2 = 328.f;
    const float RULEW = 280.f;
    const float V1 = C1 + 100.f;
    const float V2 = C2 + 100.f;
    const float LH = LINE_H - 1.f;
    const float GAP = 6.f;

    float y1 = CONTENT_Y + 4.f;
    float y2 = CONTENT_Y + 4.f;

    VLine(C2 - 10.f, CONTENT_Y + 2.f, BOT_BAR_Y - 24.f, COL_BORDER);

    // ---- LEFT: CPU ----
    DrawSection(C1, y1, RULEW, "CPU");                          y1 += LH + 4.f;
    DrawRow(C1, y1, V1, "SPEED  :", d.cpuSpeedMHz, COL_WHITE); y1 += LH;
    DrawRow(C1, y1, V1, "BRAND  :", d.cpuBrand, COL_DIM);      y1 += LH;
    DrawRow(C1, y1, V1, "CPUID  :", d.cpuLeaf1, COL_DIM);      y1 += LH + GAP;

    // ---- LEFT: Memory ----
    DrawSection(C1, y1, RULEW, "MEMORY");                       y1 += LH + 4.f;
    DrawRow(C1, y1, V1, "TOTAL  :", d.memTotal, COL_CYAN);    y1 += LH;
    DrawRow(C1, y1, V1, "AVAIL  :", d.memAvail, COL_WHITE);   y1 += LH;
    DrawRow(C1, y1, V1, "CONFIG :", d.memConfig, COL_WHITE);   y1 += LH + GAP;

    // ---- LEFT: Chipset + Revision ----
    DrawSection(C1, y1, RULEW, "CHIPSET / REVISION");           y1 += LH + 4.f;
    DrawRow(C1, y1, V1, "DEVICE :", d.chipDevId, COL_CYAN);    y1 += LH;
    DrawRow(C1, y1, V1, "BIOS   :", d.biosVer, COL_WHITE);   y1 += LH;
    DrawRow(C1, y1, V1, "SERIAL :", d.serialNum, COL_WHITE);   y1 += LH;

    // Revision row — final string in green, raw PIC bytes dimmed alongside
    DrawText(C1, y1, "REV    :", 1.2f, COL_GRAY);
    DrawText(V1, y1, d.boardRevFinal, 1.3f, COL_GREEN);
    {
        float rx = V1 + TW(d.boardRevFinal, 1.3f) + 8.f;
        char picHint[12];
        StrCopy(picHint, sizeof(picHint), "(PIC:");
        StrCat2(picHint, sizeof(picHint), picHint, d.boardRevPIC);
        StrCat2(picHint, sizeof(picHint), picHint, ")");
        DrawText(rx, y1, picHint, 1.0f, COL_DIM);
    }
    y1 += LH;

    // X-RTC expansion module
    DrawRow(C1, y1, V1, "X-RTC  :", d.rtcPresent ? "Present" : "Not detected",
        d.rtcPresent ? COL_GREEN : COL_DIM);                y1 += LH + GAP;

    // I2C display
    if (d.dispPresent)
    {
        char dispLbl[20];
        StrCat2(dispLbl, sizeof(dispLbl), "Display @ ", d.dispAddr);
        DrawRow(C1, y1, V1, "DISPLAY:", dispLbl, COL_GREEN);
    }
    else
    {
        DrawRow(C1, y1, V1, "DISPLAY:", "Not detected", COL_DIM);
    }
    y1 += LH + GAP;

    // Modchip
    {
        bool hasChip = (d.modchipName[0] != 'N'); // "None / TSOP" starts with N
        DrawRow(C1, y1, V1, "MODCHIP:", d.modchipName,
            hasChip ? COL_GREEN : COL_DIM);
        y1 += LH + GAP;
    }

    // HD mod / HDMI adapter
    {
        bool hasHdMod = (d.hdModVer[0] == 'V');
        DrawRow(C1, y1, V1, "HD MOD :", d.hdModVer,
            hasHdMod ? COL_GREEN : COL_DIM);
        y1 += LH + GAP;
    }
    DrawSection(C2, y2, RULEW, "VIDEO");                         y2 += LH + 4.f;
    DrawRow(C2, y2, V2, "ENCODER:", d.encName, COL_CYAN);       y2 += LH;
    DrawRow(C2, y2, V2, "ENC ID :", d.encId, COL_WHITE);      y2 += LH;
    DrawRow(C2, y2, V2, "AV PACK:", d.avPack, COL_WHITE);      y2 += LH + GAP;

    // ---- RIGHT: Thermal ----
    DrawSection(C2, y2, RULEW, "THERMAL");                       y2 += LH + 4.f;
    // Color code temps
    DWORD ambCol = d.tempOK
        ? ((d.rawTempAmbient > 65) ? COL_RED
            : (d.rawTempAmbient > 50) ? COL_ORANGE : COL_GREEN)
        : COL_RED;
    DWORD cpuCol = d.tempOK
        ? ((d.rawTempCPU > 80) ? COL_RED
            : (d.rawTempCPU > 70) ? COL_ORANGE : COL_GREEN)
        : COL_RED;
    DrawRow(C2, y2, V2, "AMBIENT:", d.tempAmbient, ambCol);     y2 += LH;
    DrawRow(C2, y2, V2, "CPU DIE:", d.tempCPU, cpuCol);     y2 += LH + GAP;

    // ---- RIGHT: Storage ----
    DrawSection(C2, y2, RULEW, "STORAGE");                       y2 += LH + 4.f;
    DrawRow(C2, y2, V2, "MODEL  :", d.hddModel, COL_CYAN);     y2 += LH;
    DrawRow(C2, y2, V2, "SERIAL :", d.hddSerial, COL_WHITE);    y2 += LH;
    DrawRow(C2, y2, V2, "SIZE   :", d.hddSizeGB, COL_WHITE);    y2 += LH;
    DrawRow(C2, y2, V2, "MODE   :", d.hddUDMA, COL_WHITE);    y2 += LH + GAP;

    // ---- RIGHT: DVD drive ----

    // ---- RIGHT: Network ----
    DrawSection(C2, y2, RULEW, "NETWORK");                       y2 += LH + 4.f;
    DrawRow(C2, y2, V2, "MAC    :", d.macAddr,
        d.macOK ? COL_CYAN : COL_RED);           y2 += LH;
    DrawRow(C2, y2, V2, "IP     :", d.ipAddr,
        d.ipOK ? COL_GREEN : COL_DIM);

    // ---- BOTTOM STRIP: CPU / GPU speed (text only) ----
    {
        const float STRIP_H = 22.f;
        const float STRIP_Y = BOT_BAR_Y - STRIP_H - 2.f;
        HLine(STRIP_Y, 0.f, SW, COL_BORDER);
        FillRectGrad(0.f, STRIP_Y + 1.f, SW, STRIP_Y + STRIP_H,
            D3DCOLOR_XRGB(16, 20, 44), D3DCOLOR_XRGB(10, 12, 28));
        float sy = STRIP_Y + (STRIP_H - LINE_H) * 0.5f + 1.f;
        DrawText(LM, sy, "CPU:", 1.2f, COL_GRAY);
        DrawText(LM + 34.f, sy, d.cpuSpeedMHz, 1.2f, COL_CYAN);
        float gpuX = SW * 0.5f + 8.f;
        DrawText(gpuX, sy, "GPU:", 1.2f, COL_GRAY);
        DrawText(gpuX + 34.f, sy, d.gpuSpeedMHz, 1.2f, COL_CYAN);
    }

    if (s_flashPopupOpen)
        DrawFlashPopup();

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// Tick
// ============================================================================

void SysInfo_Tick(const DiagLogo& logo)
{
    // First frame: render loading screen, then collect on second frame.
    if (!s_dataLoaded)
    {
        Render(logo);           // shows "Reading hardware..."
        ReadSysData();
        s_dataLoaded = true;
        return;
    }

    WORD cur = GetButtons();

    if (s_flashPopupOpen)
    {
        // While popup is open only B closes it; all other buttons absorbed
        if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
            s_flashPopupOpen = false;
        s_prevBtns = cur;
        Render(logo);
        return;
    }

    if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
    {
        RequestState(MSTATE_MENU);
        s_prevBtns = cur;
        return;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_A))
        ExportSysInfo();
    if (EdgeDown(cur, s_prevBtns, BTN_X))
        s_flashPopupOpen = true;
    if (EdgeDown(cur, s_prevBtns, BTN_Y))
        DumpBios();

    s_prevBtns = cur;
    Render(logo);
}

// ============================================================================
// Cross-module export
// ============================================================================

// Returns the cached board revision string from the last SysInfo data load,
// e.g. "1.0", "1.4", "1.6", "1.6b", "UNKNOWN", or "" if never loaded.
// Used by SmBusScan to detect 1.6/1.6b and apply safe scan behaviour.
const char* SysInfo_GetBoardRev()
{
    if (!s_dataLoaded) return "";
    return s_data.boardRevFinal;
}

void SysInfo_GetLCDData(LCDData& out)
{
    if (!s_dataLoaded)
    {
        out.boardRev = "";
        out.modchipName = "";
        out.cpuSpeedMHz = "";
        out.gpuSpeedMHz = "";
        out.hddModel = "";
        out.hddSizeGB = "";
        out.hddUDMA = "";
        out.ipAddr = "";
        out.macAddr = "";
        return;
    }
    out.boardRev = s_data.boardRevFinal;
    out.modchipName = s_data.modchipName;
    out.cpuSpeedMHz = s_data.cpuSpeedMHz;
    out.gpuSpeedMHz = s_data.gpuSpeedMHz;
    out.hddModel = s_data.hddModel;
    out.hddSizeGB = s_data.hddSizeGB;
    out.hddUDMA = s_data.hddUDMA;
    out.ipAddr = s_data.ipAddr;
    out.macAddr = s_data.macAddr;
}
// ============================================================================
// AutoRun — headless data gather + report write for XbSet automation
// ============================================================================

// Reuses WriteLine which is defined earlier in this file
void SysInfo_AutoRun(HANDLE hReport)
{
    // Trigger a full data read
    ReadSysData();
    s_dataLoaded = true;

    const SysData& d = s_data;

    WriteLine(hReport, "CPU IC:        ", d.cpuIC);
    WriteLine(hReport, "CPU Speed:     ", d.cpuSpeedMHz);
    WriteLine(hReport, "GPU Speed:     ", d.gpuSpeedMHz);
    WriteLine(hReport, "CPU Brand:     ", d.cpuBrand);
    WriteLine(hReport, "Mem Total:     ", d.memTotal);
    WriteLine(hReport, "Mem Config:    ", d.memConfig);
    WriteLine(hReport, "Board Rev:     ", d.boardRevFinal);
    WriteLine(hReport, "Serial:        ", d.serialNum);
    WriteLine(hReport, "Modchip:       ", d.modchipName);
    WriteLine(hReport, "HD Mod:        ", d.hdModVer);
    WriteLine(hReport, "Encoder:       ", d.encName);
    WriteLine(hReport, "AV Pack:       ", d.avPack);
    WriteLine(hReport, "Temp CPU:      ", d.tempCPU);
    WriteLine(hReport, "Temp Ambient:  ", d.tempAmbient);
    WriteLine(hReport, "HDD Model:     ", d.hddModel);
    WriteLine(hReport, "HDD Size:      ", d.hddSizeGB);
    WriteLine(hReport, "HDD UDMA:      ", d.hddUDMA);
    WriteLine(hReport, "MAC:           ", d.macAddr);
    WriteLine(hReport, "IP:            ", d.ipAddr);
}