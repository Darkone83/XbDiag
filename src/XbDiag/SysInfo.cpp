// SysInfo.cpp
// XbDiag - System Information module
//
// All hardware data read once in OnEnter().
//
// Layout:
//   Left column:  CPU / Memory / Chipset+Revision
//   Right column: Video / Thermal / Storage / Network
//   Bottom strip: CPU MHz | GPU MHz (live color bar)
//
// Revision detection (PrometheOS getHardwareRevision method):
//   1. PIC reg 0x01 x3 reads  -> 3-char board string (P01/P05/P11/P2L)
//   2. Encoder SMBus probe    -> splits P11 into 1.2/1.3 vs 1.4/1.5
//   3. NV2A PCI rev byte      -> splits 1.2 vs 1.3 (0xA1 / 0xA2)
//   4. Focus chip ID reg 0x00 -> splits 1.4 vs 1.5 (0x54 / 0x09)
//   5. NV2A EMRS RAM strap    -> splits P2L into 1.6 vs 1.6b
//
// CPU speed: measured via TSC delta over a 100ms GetTickCount() window.
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
// [A] Export to D:\sysinfo.txt     [B] Back
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

    // Memory
    char memTotal[12];
    char memAvail[12];
    char memConfig[32];

    // Chipset + board revision
    char chipDevId[12];
    char chipRev[8];
    char boardRevPIC[4];        // raw 3-char PIC string  (P01/P05/P11/P2L/DBG...)
    char boardRevFinal[16];     // human-readable: "1.0", "1.4/1.5", "1.6b" etc.
    char serialNum[16];         // raw EEPROM serial string
    bool  rtcPresent;           // X-RTC module at SMBus 0x68
    bool  dispPresent;          // I2C display detected at 0x27, 0x3C, or 0x3D
    char  dispAddr[8];          // e.g. "0x3C"
    char  modchipName[20];      // e.g. "Aladdin 1MB", "Modxo", "None/TSOP"
    char  hdModVer[16];         // e.g. "V1.2.3" or "Not Detected"

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

    // DVD drive — presence detection only (ATAPI IDENTIFY response unreliable)
    char dvdStatus[16];     // "DETECTED" or "NOT DETECTED"
    bool dvdPresent;

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
};

static SysData  s_data;
static WORD     s_prevBtns = 0;
static bool     s_dataLoaded = false;

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
// TSC-based CPU frequency measurement
// Read TSC delta over a ~100ms wall-clock window.
// Returns MHz.
// ============================================================================

static DWORD MeasureCpuMHz()
{
    DWORD lo0, hi0, lo1, hi1;

    // Wait for a tick boundary - cap iterations to prevent wedge on real hw
    DWORD tickStart = GetTickCount();
    DWORD deadline = tickStart + 50;
    bool  gotEdge = false;
    for (int spin = 0; spin < 5000000; ++spin)
    {
        DWORD t = GetTickCount();
        if (t != tickStart) { tickStart = t; gotEdge = true; break; }
        if (t > deadline)   break;
    }
    if (!gotEdge) return 733;  // timer not advancing

    __asm
    {
        rdtsc
        mov lo0, eax
        mov hi0, edx
    }

    // Wait ~100ms with hard ceiling of 500ms
    deadline = tickStart + 500;
    while ((GetTickCount() - tickStart) < 100)
    {
        if (GetTickCount() > deadline) break;
    }
    DWORD tickEnd = GetTickCount();

    __asm
    {
        rdtsc
        mov lo1, eax
        mov hi1, edx
    }

    DWORD elapsed = tickEnd - tickStart;   // ms
    if (elapsed == 0) return 733;

    // MHz = TSC delta / (elapsed_ms * 1000)
    DWORD tscDelta = lo1 - lo0;
    DWORD mhz = tscDelta / (elapsed * 1000);
    // Sanity check - Xbox CPU range is roughly 500-1400MHz
    if (mhz < 400 || mhz > 2000) return 733;
    return mhz;
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

    // Crystal = 16666 KHz
    // F_out (KHz) = (16666 * N) / (M * (1 << P))
    DWORD fKHz = (16666 * N) / (M * (1 << P));
    DWORD mhz = fKHz / 1000;

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
// P11 disambiguation (PrometheOS method):
//   Probe Conexant encoder (SMBADDR_ENC_CNXT 0x8A):
//     ACK -> 1.2 or 1.3.  Further: NV2A PCI revision byte (reg 0x08 bits[7:0])
//              0xA1 -> 1.2    0xA2 -> 1.3    other -> 1.2/1.3
//   Probe Focus encoder (SMBADDR_ENC_FOCUS 0xD4):
//     ACK -> 1.4 or 1.5.  Further: Focus chip ID reg 0x00
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
    d.boardRevPIC[3] = ' ';

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

        // Probe Conexant (0x8A) - present on 1.2 and 1.3
        if (SMBusRead(SMBADDR_ENC_CNXT, 0x00, encId))
        {
            // NV2A-A1 (0xA1) -> 1.2    NV2A-A2 (0xA2) -> 1.3
            if (nvRev == 0xA1) StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), "1.2");
            else if (nvRev == 0xA2) StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), "1.3");
            else                    StrCopy(d.boardRevFinal, sizeof(d.boardRevFinal), "1.2/1.3");
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
// Modchip detection  (ref: PrometheOS modchip::detectModchip)
// Uses direct port I/O reads at known modchip LPC signature addresses.
// Returns a static string — do not free.
// IMPORTANT: unknown result means TSOP/no modchip, NOT a probe failure.
// ============================================================================

static const char* DetectModchip()
{
    // Each probe: read a port, check for a known signature byte.
    // All inline __asm per-instruction (no __ftol2_sse concern here, but
    // keeping consistent with project style).
    BYTE val = 0;

    // Xecuter: port 0xF500 == 0xE1
    __asm { mov dx, 0xF500 }
    __asm { in  al, dx     }
    __asm { mov val, al    }
    if (val == 0xE1) return "Xecuter";

    // Modxo: port 0xDEAD == 0xAF
    __asm { mov dx, 0xDEAD }
    __asm { in  al, dx     }
    __asm { mov val, al    }
    if (val == 0xAF) return "Modxo";

    // Aladdin 1MB Lattice: port 0xF701 == 0x11
    __asm { mov dx, 0xF701 }
    __asm { in  al, dx     }
    __asm { mov val, al    }
    if (val == 0x11) return "Aladdin 1MB";

    // Aladdin 1MB Xilinx: port 0xF701 == 0x15
    if (val == 0x15) return "Aladdin 1MB";

    // Aladdin 2MB Lattice: port 0xF701 == 0x69
    if (val == 0x69) return "Aladdin 2MB";

    // Xchanger: port 0x1912 != 0xFF
    __asm { mov dx, 0x1912 }
    __asm { in  al, dx     }
    __asm { mov val, al    }
    if (val != 0xFF) return "Xchanger";

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
    // Probe: write then read reg 0 at sw-addr 0x88 (7-bit 0x44)
    // PrometheOS probes with a write first to wake the device.
    BYTE dummy = 0;
    bool probeOK = SMBusWrite(0x88, 0x00, 0x00) && SMBusRead(0x88, 0x00, dummy);

    BYTE busAddr = 0x88;
    if (!probeOK)
    {
        // Try secondary address 0x86 (7-bit 0x43) used by some adapters
        probeOK = SMBusWrite(0x86, 0x00, 0x00) && SMBusRead(0x86, 0x00, dummy);
        if (!probeOK)
        {
            StrCopy(d.hdModVer, sizeof(d.hdModVer), "Not Detected");
            return;
        }
        busAddr = 0x86;
    }

    // Read version regs 0x57/0x58/0x59
    BYTE v1 = 0, v2 = 0, v3 = 0;
    SMBusRead(busAddr, 0x57, v1);
    SMBusRead(busAddr, 0x58, v2);
    SMBusRead(busAddr, 0x59, v3);

    // Build "V%d.%d.%d" — use separate src buffer to avoid StrCat2 self-alias
    char tmp[16];
    char t[8];
    StrCopy(tmp, sizeof(tmp), "V");
    IntToStr(v1, t, sizeof(t)); StrCat2(tmp, sizeof(tmp), tmp, t);
    StrCat2(tmp, sizeof(tmp), tmp, ".");
    IntToStr(v2, t, sizeof(t)); StrCat2(tmp, sizeof(tmp), tmp, t);
    StrCat2(tmp, sizeof(tmp), tmp, ".");
    IntToStr(v3, t, sizeof(t)); StrCat2(tmp, sizeof(tmp), tmp, t);
    StrCopy(d.hdModVer, sizeof(d.hdModVer), tmp);
}

// ============================================================================
// Read all hardware data
// ============================================================================

static void ReadSysData()
{
    SysData& d = s_data;

    // --- xemu / emulator detection ---
    // Vendor string from CPUID leaf 0: EBX:EDX:ECX
    // Real Xbox Intel:  "GenuineIntel"  EBX=0x756E6547 EDX=0x49656E69 ECX=0x6C65746E
    // xemu/KVM:         "KVMKVMKVM\0\0\0" or host CPU vendor string
    // We check all three registers — EBX alone is not sufficient.
    {
        DWORD ea, eb, ec, ed;
        DoCpuid(0, ea, eb, ec, ed);
        bool isGenuineIntel = (eb == 0x756E6547UL &&   // "Genu"
            ed == 0x49656E69UL &&   // "ineI"
            ec == 0x6C65746EUL);    // "ntel"
        d.isXemu = !isGenuineIntel;
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
    // Under xemu: show the brand string directly with * marker.
    // On real hardware: derive from CPUID leaf 1 family/model.
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
        BYTE cpuFamily = (BYTE)((ea1 >> 8) & 0x0F);
        BYTE cpuModel = (BYTE)((ea1 >> 4) & 0x0F);
        BYTE cpuStepping = (BYTE)(ea1 & 0x0F);
        (void)cpuStepping;

        // Scan brand string for "Celeron"
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
            StrCopy(d.cpuIC, sizeof(d.cpuIC), "Unknown CPU");
        }
    }

    // --- CPU speed (TSC measurement) ---
    {
        d.cpuMHz = MeasureCpuMHz();
        // If running in xemu the TSC may not reflect real hardware speed
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
            // UDMA: word 88 high byte = active bits, low byte = supported bits.
            // Active bits are zero at IDENTIFY time if SET FEATURES hasn't been
            // issued yet (normal on cold-boot ATA). Fall back to supported bits
            // and mark with '*'. If word 88 is entirely zero, fall back to MWDMA
            // (word 63 high byte), then PIO.
            BYTE active = (BYTE)(buf[88] >> 8) & 0x7F;
            BYTE supported = (BYTE)(buf[88] & 0xFF) & 0x7F;
            int  mode = -1;
            bool isFallback = false;

            for (int i = 6; i >= 0; --i)
                if (active & (1 << i)) { mode = i; break; }

            if (mode < 0)
            {
                // Active bits zero — use supported bits
                for (int i = 6; i >= 0; --i)
                    if (supported & (1 << i)) { mode = i; isFallback = true; break; }
            }

            if (mode >= 0)
            {
                char t2[4]; IntToStr(mode, t2, sizeof(t2));
                StrCopy(d.hddUDMA, sizeof(d.hddUDMA), "UDMA");
                StrCat2(d.hddUDMA, sizeof(d.hddUDMA), d.hddUDMA, t2);
                if (isFallback) StrCat2(d.hddUDMA, sizeof(d.hddUDMA), d.hddUDMA, "*");
            }
            else
            {
                // Try MWDMA (word 63 high byte = active, low byte = supported)
                BYTE mwActive = (BYTE)(buf[63] >> 8) & 0x07;
                BYTE mwSupp = (BYTE)(buf[63] & 0xFF) & 0x07;
                int  mwMode = -1;
                for (int i = 2; i >= 0; --i)
                    if (mwActive & (1 << i)) { mwMode = i; break; }
                if (mwMode < 0)
                    for (int i = 2; i >= 0; --i)
                        if (mwSupp & (1 << i)) { mwMode = i; isFallback = true; break; }

                if (mwMode >= 0)
                {
                    char t2[4]; IntToStr(mwMode, t2, sizeof(t2));
                    StrCopy(d.hddUDMA, sizeof(d.hddUDMA), "MWDMA");
                    StrCat2(d.hddUDMA, sizeof(d.hddUDMA), d.hddUDMA, t2);
                    if (isFallback) StrCat2(d.hddUDMA, sizeof(d.hddUDMA), d.hddUDMA, "*");
                }
                else StrCopy(d.hddUDMA, sizeof(d.hddUDMA), "PIO");
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

    // --- DVD drive (ATAPI IDENTIFY PACKET DEVICE, secondary channel 0x170, master) ---
    // Xbox optical drive is ATAPI (secondary IDE, master, 0xA0).
    // Command 0xA1 = IDENTIFY PACKET DEVICE (ATAPI).  Returns 256 words same as ATA IDENTIFY.
    // Words [27..46] = model string (big-endian byte pairs, swap per word).
    // Words [23..26] = firmware revision string.
    {
        WORD buf[256];
        ZeroMemory(buf, sizeof(buf));
        d.dvdPresent = false;

        BYTE status = 0;
        int  timeout = 0;

        // Wait for BSY clear on secondary status register (0x177)
        timeout = 10000;
        do {
            __asm { mov dx, 0x0177 }
            __asm { in al, dx }
            __asm { mov status, al }
        } while ((status & 0x80) && --timeout > 0);

        if (timeout > 0)
        {
            // Select secondary master (nDEV=0, LBA mode)
            __asm { mov dx, 0x0176 }
            __asm { mov al, 0xA0 }
            __asm { out dx, al }

            // Issue IDENTIFY PACKET DEVICE (0xA1)
            __asm { mov dx, 0x0177 }
            __asm { mov al, 0xA1 }
            __asm { out dx, al }

            // Wait for DRQ (bit 3) set or ERR (bit 0) set
            timeout = 50000;
            do {
                __asm { mov dx, 0x0177 }
                __asm { in al, dx }
                __asm { mov status, al }
            } while (!(status & 0x08) && !(status & 0x01) && --timeout > 0);

            if ((status & 0x08) && !(status & 0x01))
            {
                // DRQ set, no error — read 256 words from data register (0x170)
                WORD* p = buf;
                for (int i = 0; i < 256; ++i)
                {
                    WORD w = 0;
                    __asm { mov dx, 0x0170 }
                    __asm { in ax, dx }
                    __asm { mov w, ax }
                    *p++ = w;
                }
                d.dvdPresent = true;
                StrCopy(d.dvdStatus, sizeof(d.dvdStatus), "DETECTED");
            }
        }

        if (!d.dvdPresent)
            StrCopy(d.dvdStatus, sizeof(d.dvdStatus), "NOT DETECTED");
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
// The Xbox flash ROM is memory-mapped read-only at the top of the 32-bit
// address space. Common sizes:
//   256KB retail BIOS: mapped at 0xFFFC0000 - 0xFFFFFFFF
//   1MB mod chip BIOS: mapped at 0xFFF00000 - 0xFFFFFFFF
//
// Detection: if the first 256KB bank at 0xFFF00000 matches the bank at
// 0xFFFC0000 it is a 256KB chip (ROM tiled to fill 1MB address window).
// If they differ it is a genuine 1MB image - dump the full megabyte.
// Output: D:\bios.bin (XBE launch directory, always writable).
// ============================================================================

static void DumpBios()
{
    SysData& d = s_data;

    const BYTE* base256 = (const BYTE*)0xFFFC0000UL; // top 256KB always valid
    const BYTE* base1M = (const BYTE*)0xFFF00000UL; // base of potential 1MB

    // Determine image size: compare first 16 bytes at each 256KB boundary.
    // On a 256KB chip the ROM is tiled four times, so the banks are identical.
    // On a 1MB chip bank 0 (0xFFF00000) holds distinct data from bank 3 (0xFFFC0000).
    bool is1MB = false;
    for (int bi = 0; bi < 16; ++bi)
    {
        if (base1M[bi] != base256[bi]) { is1MB = true; break; }
    }

    const BYTE* dumpBase = is1MB ? base1M : base256;
    DWORD       dumpSize = is1MB ? (1024UL * 1024UL) : (256UL * 1024UL);

    HANDLE hf = CreateFileA("D:\\bios.bin", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        d.biosDumpDone = true; d.biosDumpOK = false; return;
    }

    // Write in 4KB pages to avoid one massive WriteFile call
    const DWORD PAGE = 4096;
    DWORD written = 0;
    bool  ok = true;
    for (DWORD offset = 0; offset < dumpSize && ok; offset += PAGE)
    {
        DWORD chunk = (dumpSize - offset < PAGE) ? (dumpSize - offset) : PAGE;
        DWORD w = 0;
        if (!WriteFile(hf, dumpBase + offset, chunk, &w, NULL) || w != chunk)
            ok = false;
        else
            written += w;
    }

    CloseHandle(hf);
    d.biosDumpSize = written;
    d.biosDumpDone = true;
    d.biosDumpOK = ok;
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
    WriteLine(hf, "DVD Drive:     ", d.dvdStatus);
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
    s_data.exportDone = false;
    s_data.exportOK = false;
    s_data.biosDumpDone = false;
    s_data.biosDumpOK = false;
    s_data.biosDumpSize = 0;

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
    if (s_data.biosDumpDone)
        hint = s_data.biosDumpOK ? "[A] Export  [Y] BIOS dumped OK  [B] Back"
        : "[A] Export  [Y] BIOS dump FAIL  [B] Back";
    else if (s_data.exportDone)
        hint = s_data.exportOK ? "[A] Exported OK  [Y] Dump BIOS  [B] Back"
        : "[A] Export fail  [Y] Dump BIOS  [B] Back";
    else
        hint = "[A] Export    [Y] Dump BIOS    [B] Back";

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
    DrawRow(C1, y1, V1, "CPU    :", d.cpuIC, COL_CYAN);  y1 += LH;
    DrawRow(C1, y1, V1, "SPEED  :", d.cpuSpeedMHz, COL_WHITE); y1 += LH;
    DrawRow(C1, y1, V1, "BRAND  :", d.cpuBrand,
        d.isXemu ? COL_YELLOW : COL_DIM);                       y1 += LH + GAP;

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
    DrawSection(C2, y2, RULEW, "DVD DRIVE");                     y2 += LH + 4.f;
    DrawRow(C2, y2, V2, "STATUS :", d.dvdStatus,
        d.dvdPresent ? COL_GREEN : COL_RED);                 y2 += LH + GAP;

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

    if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
    {
        RequestState(MSTATE_MENU);
        s_prevBtns = cur;
        return;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_A))
        ExportSysInfo();
    if (EdgeDown(cur, s_prevBtns, BTN_Y))
        DumpBios();

    s_prevBtns = cur;
    Render(logo);
}