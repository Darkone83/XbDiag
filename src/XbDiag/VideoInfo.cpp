// VideoInfo.cpp
// XbDiag - Video Info
//
// Layout (640x480 design space):
//
//  [TOP BAR]
//  ---------------------------------------------------------------
//  LEFT COLUMN                    RIGHT COLUMN
//
//  DISPLAY                        ENCODER
//  Video Mode    480i / 720p etc  Chip          CX25871 / FS454 etc
//  Resolution    640x480 etc      Chip ID       0x76 etc
//  Scale X       1.00             AV Pack       Composite / HDTV etc
//  Scale Y       1.00
//  HD Mode       YES / NO
//
//  FRAMEBUFFER
//  Width         640
//  Height        480
//  Color Depth   32bpp
//  Refresh Rate  60 Hz (estimated)
//
//  [BOT BAR]  [X] NTSC color bars    [Y] PAL color bars    [White] Mode test    [B] Back
//
// Sources:
//   Video mode / resolution  - g_videoModeStr, g_sx, g_sy, backbuffer desc
//   Encoder chip             - SMBus 0x45 (Conexant, sw 0x8A) / 0x6A (Focus, sw 0xD4) reg 0x00 (ID byte)
//   AV pack                  - SMBus 0x10 (PIC16L, sw-shifted 0x20) reg 0x04 bits [2:0]

#include "VideoInfo.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

// ============================================================================
// Layout
// ============================================================================

static const float CY = CONTENT_Y + 6.f;
static const float COL_L = LM;
static const float COL_R = SW * 0.5f + 4.f;
static const float COL_VL = COL_L + 110.f;
static const float COL_VR = COL_R + 64.f;   // was +110; tightened to fit longest value at LM=48
static const float LH = LINE_H - 1.f;
static const float GAP = 8.f;

// ============================================================================
// Data
// ============================================================================

struct VideoData
{
    // From D3D / globals
    char modeStr[16];    // "480i" / "480p" / "720p" / "1080i" / "576i PAL"
    char resStr[16];     // "640x480"
    char scaleX[10];     // "1.00"
    char scaleY[10];     // "1.50" etc
    char isHDStr[6];     // "YES" / "NO"
    int  bbWidth;
    int  bbHeight;
    char bppStr[8];      // "32bpp"
    char fmtStr[16];     // "X8R8G8B8" etc
    char refreshStr[12]; // "60 Hz" / "50 Hz"

    // Encoder (SMBus 0x45/0x6A Conexant/Focus, reg 0x00 = chip ID)
    bool encAck;
    BYTE encIdByte;
    char encName[28];    // "Conexant CX25871" etc
    char encIdStr[8];    // "0x76" etc

    // Encoder output standard (from encoder reg 0x02 / 0x01)
    char encStd[16];     // "NTSC" / "PAL-B/G" / "PAL-M" / "PAL-N" / "N/A"

    // AV pack (SMBus 0x10 PIC16L, reg 0x04 bits [2:0])
    char avPack[24];     // "Composite" / "HDTV" / "S-Video" etc

    // HD mod / HDMI adapter (SMBus 0x44 / 0x43, Chimeric-compatible)
    char hdModVer[16];   // "V1.2.3" or "" if not detected

    // NV2A registers (MMIO 0xFD000000)
    bool nv2aOK;
    char gpuClkStr[16];  // "233 MHz" (NVPLL)
    char memClkStr[16];  // "200 MHz" (MPLL x2 DDR)
    char pixClkStr[12];  // "74 MHz" (VPLL)
    char fbBaseStr[12];  // "0x00FC0000"
    char vramStr[8];     // "64 MB" / "128 MB"
    // Raw register values for field diagnosis
    char nvpllRaw[12];   // "0x00011C01"
    char mpllRaw[12];    // "0x0001...."
};

static VideoData s_data;
static WORD      s_prevBtns;
static bool      s_loaded = false;

enum VideoSubState { VSS_INFO, VSS_NTSC_BARS, VSS_PAL_BARS, VSS_MODE_TEST };
static VideoSubState s_subState = VSS_INFO;

// ============================================================================
// Mode test — state, probe table, and mode table
// ============================================================================
//
// ModeEntry mirrors main.cpp's VideoMode but is local to VideoInfo.
// needPack: 0 = any AV pack,  1 = 480p-capable (HDTV or VGA),  2 = HDTV-only
//
// AV pack byte (PIC reg 0x04 bits [2:0]):
//   0=SCART  1=HDTV  2=VGA  4=S-Video  5=Composite  6=None  0xFF=unknown
//
// After a mode switch g_sx, g_sy, g_videoModeStr and g_isHD are updated so
// all DiagCommon drawing primitives (FillRect, DrawText etc.) remain correct.
// On exit, RestoreMode() resets back to the original presentation parameters
// and re-syncs those same globals.
//
// Render states are explicitly re-applied after every Reset() because the
// device loses all state — the four lines mirror InitD3D() in main.cpp.
//
// NOTE: Xbox D3D8 Reset() does not invalidate CPU-side texture allocations
// (unified memory — no "default pool" eviction as on PC D3D8).  The logo
// texture (g_logo.tex) and font (pure DrawPrimitiveUP, no texture) survive.
//
// Three-layer capability model:
//   Layer 1 — policy eligibility  (AV pack + region + EEPROM flags)
//   Layer 2 — device acceptance   (Reset() succeeded)
//   Layer 3 — BB verification     (backbuffer dimensions match request)
//
// All five modes are always shown. Blocked modes are dimmed with a reason.
// [A] Force-try lets the user attempt a blocked mode manually.
//
// AV pack byte (PIC reg 0x04 bits [2:0]):
//   0=SCART  1=HDTV  2=VGA  4=S-Video  5=Composite  6=None  0xFF=unknown
//
// EEPROM video flags (XC_VIDEO_FLAGS kernel setting ID 0x05):
//   XC_VIDEO_FLAGS_HDTV_720p  = 0x00000002
//   XC_VIDEO_FLAGS_HDTV_1080i = 0x00000004
//   XC_VIDEO_FLAGS_HDTV_480p  = 0x00000008
// These are the values returned by ExQueryNonVolatileSetting(XC_VIDEO_FLAGS).
// The raw EEPROM byte layout at offset 0x94 uses different bit positions.

struct ModeEntry
{
    const char* label;
    DWORD       width;
    DWORD       height;
    DWORD       presentFlags;
    DWORD       refreshHz;
    bool        isPAL;
    BYTE        needPack;     // 0=any  1=480p-capable  2=HDTV-only
};

static const ModeEntry s_allModes[] =
{
    { "480i",     640,  480, D3DPRESENTFLAG_INTERLACED,                             60, false, 0 },
    { "480p",     640,  480, D3DPRESENTFLAG_PROGRESSIVE,                            60, false, 1 },
    { "576i PAL", 720,  576, D3DPRESENTFLAG_INTERLACED,                             50, true,  0 },
    { "720p",    1280,  720, D3DPRESENTFLAG_PROGRESSIVE | D3DPRESENTFLAG_WIDESCREEN, 60, false, 2 },
    { "1080i",   1920, 1080, D3DPRESENTFLAG_INTERLACED | D3DPRESENTFLAG_WIDESCREEN, 60, false, 2 },
};
static const int ALL_MODE_COUNT = sizeof(s_allModes) / sizeof(s_allModes[0]);

// Per-mode probe status — built at OnEnter, never changes during the session
enum ProbeStatus
{
    PROBE_SUPPORTED = 0,    // all checks pass — mode is selectable
    PROBE_BLOCKED_PACK,     // AV pack doesn't support this mode
    PROBE_BLOCKED_REGION,   // video region (PAL/NTSC) doesn't match
    PROBE_BLOCKED_EEPROM,   // EEPROM HD-enable flag is off
    PROBE_UNKNOWN,          // couldn't read EEPROM — indeterminate
};

struct ModeProbe
{
    ProbeStatus status;
    char        reason[32]; // shown in mode list for blocked modes
};

static ModeProbe s_probes[ALL_MODE_COUNT];

// Filtered list of selectable (PROBE_SUPPORTED) modes for cycling
static int s_modeList[ALL_MODE_COUNT];  // indices into s_allModes
static int s_modeCount = 0;
static int s_modeIdx = 0;         // index into s_modeList (current active)

// For forced test of blocked modes
static bool s_forceMode = false;  // true if current switch was a forced trial
static int  s_forceAllIdx = -1;   // s_allModes index being force-tried

static BYTE  s_avPackTest = 0xFF;
static DWORD s_eepVideoFlags = 0;
static bool  s_eepFlagsOK = false;  // false if EEPROM read failed
static int   s_settleFrames = 0;
static bool  s_resetOK = false;    // result of most recent SwitchMode Reset()
static DWORD s_bbW = 0, s_bbH = 0; // backbuffer dims after last switch

// Encoder type detected at OnEnter — governs whether EncReprogramMode can work
enum EncType { ENC_UNKNOWN = 0, ENC_CONEXANT, ENC_FOCUS, ENC_XCALIBUR };
static EncType s_encType = ENC_UNKNOWN;

// Auto-restore timeout — if no button is pressed within 5 seconds of a mode
// switch, automatically restore 480i. Prevents permanent black screen when
// switching to a mode the connected TV can't display.
static const DWORD MODE_TIMEOUT_MS = 10000;
static DWORD s_modeTimeout = 0;    // absolute deadline, 0 = no timeout active

// Original mode
static char  s_origLabel[16] = "480i";
static DWORD s_origWidth = 640;
static DWORD s_origHeight = 480;
static DWORD s_origFlags = D3DPRESENTFLAG_INTERLACED;
static DWORD s_origRefresh = 60;

// ExQueryNonVolatileSetting — EEPROM kernel API
extern "C" LONG __stdcall ExQueryNonVolatileSetting(
    ULONG ValueIndex, ULONG* Type, void* Value, ULONG ValueLength, ULONG* ResultLength);
#define XC_VIDEO_FLAGS  0x05

// ============================================================================
// Helpers
// ============================================================================

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

static void FmtFloat2(float v, char* out, int outLen)
{
    int whole = Ftoi(v);
    int frac = Ftoi((v - (float)whole) * 100.f + 0.5f);
    if (frac >= 100) { whole += 1; frac -= 100; }
    int i = 0;
    char wbuf[8]; int wi = 0;
    int tmp = whole;
    if (tmp == 0) { wbuf[wi++] = '0'; }
    else { while (tmp > 0 && wi < 7) { wbuf[wi++] = '0' + (tmp % 10); tmp /= 10; } }
    for (int k = wi - 1; k >= 0 && i < outLen - 1; --k) out[i++] = wbuf[k];
    if (i < outLen - 1) out[i++] = '.';
    if (i < outLen - 1) out[i++] = '0' + (frac / 10);
    if (i < outLen - 1) out[i++] = '0' + (frac % 10);
    out[i] = '\0';
}

// ---- AV pack display name -------------------------------------------------
static const char* AvPackName(BYTE av)
{
    switch (av)
    {
    case 0:    return "SCART";
    case 1:    return "HDTV";
    case 2:    return "VGA";
    case 4:    return "S-VIDEO";
    case 5:    return "COMPOSITE";
    case 6:    return "NONE";
    default:   return "UNKNOWN";
    }
}

// Build per-mode probe table. Called at OnEnter after AV pack and EEPROM reads.
// Determines Layer 1 (policy) status for all five modes without switching anything.
//
// EEPROM HD-enable flags (480p/720p/1080i) are read and shown in the UI for
// information, but they are NOT used as a hard block. The real hardware gate
// is the AV pack — if the cable supports the mode, the user can test it even
// if the dashboard hasn't enabled it in EEPROM. This matches what PrometheOS
// and other dashboards do (they set those flags themselves on launch).
static void BuildProbeTable()
{
    bool can480p = (s_avPackTest == 1 || s_avPackTest == 2);
    bool canHD = (s_avPackTest == 1);

    // PAL region check
    ULONG vsType = 0, vsLen = 0;
    DWORD videoStd = 0;
    ExQueryNonVolatileSetting(0x04 /*XC_VIDEO_STANDARD*/, &vsType, &videoStd, 4, &vsLen);
    bool isPALRegion = (videoStd == 0x00800300 || videoStd == 0x00400400);

    s_modeCount = 0;

    for (int i = 0; i < ALL_MODE_COUNT; ++i)
    {
        const ModeEntry& e = s_allModes[i];
        ModeProbe& p = s_probes[i];
        p.status = PROBE_SUPPORTED;
        p.reason[0] = '\0';

        // 576i PAL — block on NTSC region
        if (e.isPAL && !isPALRegion)
        {
            p.status = PROBE_BLOCKED_REGION;
            StrCopy(p.reason, sizeof(p.reason), "NTSC REGION");
            continue;
        }

        // AV pack hard block
        if (e.needPack == 1 && !can480p)
        {
            p.status = PROBE_BLOCKED_PACK;
            StrCopy(p.reason, sizeof(p.reason), "NEEDS HDTV/VGA");
            continue;
        }
        if (e.needPack == 2 && !canHD)
        {
            p.status = PROBE_BLOCKED_PACK;
            StrCopy(p.reason, sizeof(p.reason), "NEEDS HDTV");
            continue;
        }

        // Encoder capability checks — only applied once encoder is known
        if (e.width == 1920 && e.height == 1080)
        {
            // Xcalibur: no known register map — hard block 1080i
            if (s_encType == ENC_XCALIBUR)
            {
                p.status = PROBE_BLOCKED_EEPROM;  // reuse blocked status
                StrCopy(p.reason, sizeof(p.reason), "NO XCALIBUR MAP");
                continue;
            }
            // Conexant: register values are unverified for 1080i — mark experimental
            // Still added to selectable list but flagged PROBE_UNKNOWN
            if (s_encType == ENC_CONEXANT)
            {
                p.status = PROBE_UNKNOWN;
                StrCopy(p.reason, sizeof(p.reason), "EXPERIMENTAL");
                // Fall through — still add to mode list for forced/manual test
            }
        }

        s_modeList[s_modeCount++] = i;
    }

    if (s_modeCount == 0)
    {
        s_probes[0].status = PROBE_SUPPORTED;
        s_probes[0].reason[0] = '\0';
        s_modeList[s_modeCount++] = 0;
    }
}

// ============================================================================
// NV2A MMIO helpers
// ============================================================================
// Base address of NV2A MMIO window on Xbox
#define NV2A_BASE  0xFD000000UL

extern "C" VOID __stdcall HalReadWritePCISpace(
    ULONG BusNumber, ULONG SlotNumber, ULONG RegisterNumber,
    PVOID Buffer, ULONG Length, BOOLEAN WritePCISpace);

static DWORD ViPciRead32(BYTE bus, BYTE dev, BYTE func, BYTE reg)
{
    DWORD val = 0;
    ULONG slot = ((ULONG)dev & 0x1F) | (((ULONG)func & 0x07) << 5);
    HalReadWritePCISpace(bus, slot, reg, &val, sizeof(val), FALSE);
    return val;
}

// Decode NV2A PLL register (M/N/P) to MHz with proper rounding.
// F_out = (16666 KHz * N) / (M * 2^P)
// Uses +half-divisor rounding to avoid truncation errors that cause
// known values (e.g. 100MHz SDRAM -> 200MHz DDR) to decode as 99 or 101.
static DWORD NvPllToMHz(DWORD reg)
{
    DWORD M = (reg >> 0) & 0xFF;
    DWORD N = (reg >> 8) & 0xFF;
    DWORD P = (reg >> 16) & 0x0F;
    if (M == 0 || N == 0) return 0;
    DWORD divisor = M * (1UL << P);
    DWORD fKHz = (16666UL * N + divisor / 2) / divisor;  // rounded
    return (fKHz + 500) / 1000;  // KHz -> MHz, rounded
}

static void LoadNV2A(VideoData& d)
{
    d.nv2aOK = false;
    StrCopy(d.gpuClkStr, sizeof(d.gpuClkStr), "N/A");
    StrCopy(d.memClkStr, sizeof(d.memClkStr), "N/A");
    StrCopy(d.pixClkStr, sizeof(d.pixClkStr), "N/A");
    StrCopy(d.fbBaseStr, sizeof(d.fbBaseStr), "N/A");
    StrCopy(d.vramStr, sizeof(d.vramStr), "N/A");
    StrCopy(d.nvpllRaw, sizeof(d.nvpllRaw), "0x00000000");
    StrCopy(d.mpllRaw, sizeof(d.mpllRaw), "0x00000000");

    d.nv2aOK = true;

    // ---- Helper: decode PLL register and format as MHz string ----------
    // NVPLL / MPLL format: M[7:0], N[15:8], P[19:16]
    // F_out = (16666 KHz * N) / (M * 2^P)
    // Returns 0 if register reads as zero (MMIO not mapped / not clocked).

    // ---- NVPLL: GPU core clock at PRAMDAC+0x500 (0xFD680500) -----------
    {
        volatile DWORD* pNvpll = (volatile DWORD*)(NV2A_BASE + 0x680500UL);
        DWORD reg = *pNvpll;
        // Store raw for diagnostic display
        char rh[10]; IntToHex(reg, 8, rh, sizeof(rh));
        StrCat2(d.nvpllRaw, sizeof(d.nvpllRaw), "0x", rh);

        DWORD mhz = NvPllToMHz(reg);
        if (mhz > 100 && mhz < 600)
        {
            char t[8]; IntToStr((int)mhz, t, sizeof(t));
            StrCat2(d.gpuClkStr, sizeof(d.gpuClkStr), t, " MHz");
        }
        else
        {
            // MMIO returned 0 or unexpected value — use known Xbox default
            StrCopy(d.gpuClkStr, sizeof(d.gpuClkStr), "233 MHz*");
        }
    }

    // ---- MPLL: memory clock ------------------------------------------------
    // Xbox UMA memory is always 200 MHz DDR on all retail revisions (64MB or 128MB).
    // The MPLL register uses a 13.5 MHz crystal reference, not the 16.666 MHz
    // PRAMDAC reference, so NvPllToMHz() gives nonsensical results here.
    // Report the known retail value directly and show the raw register for reference.
    {
        volatile DWORD* pMpll = (volatile DWORD*)(NV2A_BASE + 0x680504UL);
        DWORD reg = *pMpll;
        char rh[10]; IntToHex(reg, 8, rh, sizeof(rh));
        StrCopy(d.mpllRaw, sizeof(d.mpllRaw), "0x");
        StrCat2(d.mpllRaw, sizeof(d.mpllRaw), d.mpllRaw, rh);
        // All retail Xbox: 200 MHz DDR, shared UMA between CPU, GPU, and system
        StrCopy(d.memClkStr, sizeof(d.memClkStr), "200 MHz DDR");
    }

    // ---- VPLL1: pixel clock at PRAMDAC+0x508 (0xFD680508) ---------------
    {
        volatile DWORD* pVpll = (volatile DWORD*)(NV2A_BASE + 0x680508UL);
        DWORD mhz = NvPllToMHz(*pVpll);
        if (mhz > 0 && mhz < 600)
        {
            char t[8]; IntToStr((int)mhz, t, sizeof(t));
            StrCat2(d.pixClkStr, sizeof(d.pixClkStr), t, " MHz");
        }
    }

    // ---- PCRTC_START: framebuffer scanout base at 0x600810 ---------------
    {
        volatile DWORD* pFbBase = (volatile DWORD*)(NV2A_BASE + 0x600810UL);
        DWORD addr = *pFbBase;
        char t[10]; IntToHex(addr, 8, t, sizeof(t));
        StrCat2(d.fbBaseStr, sizeof(d.fbBaseStr), "0x", t);
    }

    // ---- PFB_BOOT_0: VRAM size strap at 0x100200, bit 2 -----------------
    {
        volatile DWORD* pPfb = (volatile DWORD*)(NV2A_BASE + 0x100200UL);
        DWORD boot0 = *pPfb;
        StrCopy(d.vramStr, sizeof(d.vramStr), (boot0 & (1 << 2)) ? "128 MB" : "64 MB");
    }
}

// ============================================================================
// Data loading
// ============================================================================

static void LoadData()
{
    VideoData& d = s_data;

    // ---- Video mode from globals set in main.cpp ----
    StrCopy(d.modeStr, sizeof(d.modeStr), g_videoModeStr);
    StrCopy(d.isHDStr, sizeof(d.isHDStr), g_isHD ? "YES" : "NO");

    FmtFloat2(g_sx, d.scaleX, sizeof(d.scaleX));
    FmtFloat2(g_sy, d.scaleY, sizeof(d.scaleY));

    // ---- Backbuffer dimensions via D3D ----
    d.bbWidth = 0;
    d.bbHeight = 0;
    StrCopy(d.bppStr, sizeof(d.bppStr), "32bpp");
    StrCopy(d.fmtStr, sizeof(d.fmtStr), "N/A");
    StrCopy(d.refreshStr, sizeof(d.refreshStr), "N/A");

    if (g_pDevice)
    {
        LPDIRECT3DSURFACE8 pBB = NULL;
        if (SUCCEEDED(g_pDevice->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &pBB)) && pBB)
        {
            D3DSURFACE_DESC desc;
            if (SUCCEEDED(pBB->GetDesc(&desc)))
            {
                d.bbWidth = (int)desc.Width;
                d.bbHeight = (int)desc.Height;

                // Color depth from format
                switch (desc.Format)
                {
                case D3DFMT_A8R8G8B8:
                    StrCopy(d.bppStr, sizeof(d.bppStr), "32bpp");
                    StrCopy(d.fmtStr, sizeof(d.fmtStr), "A8R8G8B8");
                    break;
                case D3DFMT_X8R8G8B8:
                    StrCopy(d.bppStr, sizeof(d.bppStr), "32bpp");
                    StrCopy(d.fmtStr, sizeof(d.fmtStr), "X8R8G8B8");
                    break;
                case D3DFMT_R5G6B5:
                    StrCopy(d.bppStr, sizeof(d.bppStr), "16bpp");
                    StrCopy(d.fmtStr, sizeof(d.fmtStr), "R5G6B5");
                    break;
                case D3DFMT_X1R5G5B5:
                    StrCopy(d.bppStr, sizeof(d.bppStr), "16bpp");
                    StrCopy(d.fmtStr, sizeof(d.fmtStr), "X1R5G5B5");
                    break;
                case D3DFMT_A1R5G5B5:
                    StrCopy(d.bppStr, sizeof(d.bppStr), "16bpp");
                    StrCopy(d.fmtStr, sizeof(d.fmtStr), "A1R5G5B5");
                    break;
                default:
                    StrCopy(d.bppStr, sizeof(d.bppStr), "?bpp");
                    StrCopy(d.fmtStr, sizeof(d.fmtStr), "?");
                    break;
                }
            }
            pBB->Release();
        }

        // Real refresh rate from D3D display mode
        {
            D3DDISPLAYMODE dm;
            ZeroMemory(&dm, sizeof(dm));
            if (SUCCEEDED(g_pDevice->GetDisplayMode(&dm)) && dm.RefreshRate > 0)
            {
                char t[8];
                IntToStr((int)dm.RefreshRate, t, sizeof(t));
                StrCopy(d.refreshStr, sizeof(d.refreshStr), t);
                StrCat2(d.refreshStr, sizeof(d.refreshStr), d.refreshStr, " Hz");
            }
            else
            {
                // Fallback: infer from mode string
                bool isPAL = (g_videoModeStr[0] == '5' &&
                    g_videoModeStr[1] == '7' &&
                    g_videoModeStr[2] == '6');
                char t[8];
                IntToStr(isPAL ? 50 : 60, t, sizeof(t));
                StrCopy(d.refreshStr, sizeof(d.refreshStr), t);
                StrCat2(d.refreshStr, sizeof(d.refreshStr), d.refreshStr, " Hz");
            }
        }
    }

    // Build res string from backbuffer (fall back to scale-derived if BB query failed)
    {
        int w = d.bbWidth ? d.bbWidth : Ftoi(640.f * g_sx);
        int h = d.bbHeight ? d.bbHeight : Ftoi(480.f * g_sy);
        char t[8];
        IntToStr(w, t, sizeof(t));
        StrCopy(d.resStr, sizeof(d.resStr), t);
        StrCat2(d.resStr, sizeof(d.resStr), d.resStr, "x");
        IntToStr(h, t, sizeof(t));
        StrCat2(d.resStr, sizeof(d.resStr), d.resStr, t);
    }

    // ---- Encoder: probe Conexant (0x8A), Focus (0xD4), Xcalibur fallback ----
    // Ref: PrometheOS getEncoderString() - tries 0x8A, then 0xD4, else Xcalibur
    BYTE encId = 0;
    d.encAck = false;
    d.encIdByte = 0;
    StrCopy(d.encStd, sizeof(d.encStd), "N/A");
    {
        char t[4];
        if (SMBusRead(SMBADDR_ENC_CNXT, 0x00, encId))
        {
            d.encAck = true;
            d.encIdByte = encId;
            IntToHex(encId, 2, t, sizeof(t));
            StrCopy(d.encIdStr, sizeof(d.encIdStr), "0x");
            StrCat2(d.encIdStr, sizeof(d.encIdStr), d.encIdStr, t);
            switch (encId)
            {
            case 0x76: StrCopy(d.encName, sizeof(d.encName), "Conexant CX25871"); break;
            default:
            {
                StrCopy(d.encName, sizeof(d.encName), "Conexant (0x");
                StrCat2(d.encName, sizeof(d.encName), d.encName, t);
                StrCat2(d.encName, sizeof(d.encName), d.encName, ")");
            }
            }
            // CX25871 reg 0x02 bits [1:0] = output standard
            BYTE stdReg = 0;
            if (SMBusRead(SMBADDR_ENC_CNXT, 0x02, stdReg))
            {
                switch (stdReg & 0x03)
                {
                case 0: StrCopy(d.encStd, sizeof(d.encStd), "NTSC");    break;
                case 1: StrCopy(d.encStd, sizeof(d.encStd), "PAL-B/G"); break;
                case 2: StrCopy(d.encStd, sizeof(d.encStd), "PAL-M");   break;
                case 3: StrCopy(d.encStd, sizeof(d.encStd), "PAL-N");   break;
                }
            }
        }
        else if (SMBusRead(SMBADDR_ENC_FOCUS, 0x00, encId))
        {
            d.encAck = true;
            d.encIdByte = encId;
            IntToHex(encId, 2, t, sizeof(t));
            StrCopy(d.encIdStr, sizeof(d.encIdStr), "0x");
            StrCat2(d.encIdStr, sizeof(d.encIdStr), d.encIdStr, t);
            switch (encId)
            {
            case 0x54: StrCopy(d.encName, sizeof(d.encName), "Focus FS454"); break;
            case 0x09: StrCopy(d.encName, sizeof(d.encName), "Focus FS455"); break;
            default:
            {
                StrCopy(d.encName, sizeof(d.encName), "Focus (0x");
                StrCat2(d.encName, sizeof(d.encName), d.encName, t);
                StrCat2(d.encName, sizeof(d.encName), d.encName, ")");
            }
            }
            // FS454/FS455 reg 0x01 bits [1:0] = output standard
            BYTE stdReg = 0;
            if (SMBusRead(SMBADDR_ENC_FOCUS, 0x01, stdReg))
            {
                switch (stdReg & 0x03)
                {
                case 0: StrCopy(d.encStd, sizeof(d.encStd), "NTSC");    break;
                case 1: StrCopy(d.encStd, sizeof(d.encStd), "PAL-B/G"); break;
                case 2: StrCopy(d.encStd, sizeof(d.encStd), "PAL-M");   break;
                case 3: StrCopy(d.encStd, sizeof(d.encStd), "PAL-N");   break;
                }
            }
        }
        else
        {
            StrCopy(d.encName, sizeof(d.encName), "Xcalibur (1.6)");
            StrCopy(d.encIdStr, sizeof(d.encIdStr), "N/A");
        }
    }

    // ---- AV pack (SMBus 0x45 reg 0x04 bits [2:0]) ----
    BYTE av = 0;
    if (SMBusRead(SMBADDR_PIC, 0x04, av))
    {
        char t[4];
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
            IntToHex(av & 0x07, 2, t, sizeof(t));
            StrCopy(d.avPack, sizeof(d.avPack), "Unknown (0x");
            StrCat2(d.avPack, sizeof(d.avPack), d.avPack, t);
            StrCat2(d.avPack, sizeof(d.avPack), d.avPack, ")");
            break;
        }
        }
    }
    else StrCopy(d.avPack, sizeof(d.avPack), "PIC NAK");

    // ---- HD mod / HDMI adapter (ref: PrometheOS xboxConfig::getHdModString) ----
    // Probe sw-addr 0x88 (7-bit 0x44) then fall back to 0x86 (7-bit 0x43).
    // Write first to wake device, then read version regs 0x57/0x58/0x59.
    {
        d.hdModVer[0] = '\0';
        BYTE dummy = 0;
        BYTE busAddr = 0;
        if (SMBusWrite(0x88, 0x00, 0x00) && SMBusRead(0x88, 0x00, dummy))
            busAddr = 0x88;
        else if (SMBusWrite(0x86, 0x00, 0x00) && SMBusRead(0x86, 0x00, dummy))
            busAddr = 0x86;

        if (busAddr != 0)
        {
            BYTE v1 = 0, v2 = 0, v3 = 0;
            SMBusRead(busAddr, 0x57, v1);
            SMBusRead(busAddr, 0x58, v2);
            SMBusRead(busAddr, 0x59, v3);
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
    }

    // ---- NV2A MMIO registers ----
    LoadNV2A(d);
}

// ============================================================================
// Color bar test patterns
// ============================================================================
//
// NTSC (SMPTE RP 219):
//   Top 2/3  - 7 bars at 75% saturation: White / Yellow / Cyan / Green /
//              Magenta / Red / Blue  (left to right)
//   Middle stripe (~1/12) - reverse blue bars + pluge
//   Bottom 1/4 - -I / White / +Q / Black / Pluge sub-black / Black / Pluge
//
// PAL (EBU Tech 3373):
//   Top 3/4  - 8 bars: White / Yellow / Cyan / Green / Magenta / Red /
//              Blue / Black
//   Bottom 1/4 - black-level reference bars (same 8 columns, alternating
//                grey/black/dark-grey)
//
// All coordinates in 640x480 design space; FillRect scales to backbuffer.

// ============================================================================
// Mode test helpers
// ============================================================================

// Forward decls
static void DrawColorBarsContent(bool isPAL);  // pure drawing, no scene mgmt

// ============================================================================
// EncReprogramMode — reprogram video encoder for the requested output mode
//
// Xbox D3D Reset() reconfigures the NV2A scanout engine but does NOT
// reprogram the encoder chip for a different output standard. Switching from
// 480i to 1080i without this step produces a black screen because the encoder
// is still outputting 480i timing.
//
// CX25871 (v1.0–1.3): 13 registers written per mode.
//   Source: Xbox kernel nkpatcher table (NESdev community reverse engineering)
//
// Focus FS454/FS455 (v1.4–1.5): single 16-bit VID_CNTL0 register at 0x92.
//   Source: Ryzee119/spi2par2019 xboxsmbus.h (measured from real hardware)
//   480i:  0x48C5    480p: 0x483E
//   720p:  0x582E    1080i: 0x50AE
//
// Xcalibur (v1.6): no known register map — skip.
// ============================================================================

// CX25871 register addresses (13 registers used by Xbox kernel)
static const BYTE s_cnxtRegs[13] =
{
    0xD6, 0x2E, 0x32, 0x3C, 0x3E, 0x40, 0xC4, 0xC6, 0xCE, 0xA0, 0x9E, 0x9C, 0x6C
};

// CX25871 values per mode: [0]=480i [1]=480p [2]=576i [3]=720p [4]=1080i
// 480i and 1080i share identical kernel values — only NV2A timing differs
static const BYTE s_cnxtVals[5][13] =
{
    // 480i
    { 0x0C, 0x00, 0x48, 0x80, 0x80, 0x80, 0x01, 0x98, 0xE1, 0x21, 0x00, 0x00, 0x46 },
    // 480p
    { 0x0C, 0x00, 0x48, 0x80, 0x80, 0x80, 0x01, 0x98, 0xE1, 0x8C, 0x00, 0x00, 0x46 },
    // 576i PAL
    { 0x0C, 0x00, 0x48, 0x80, 0x80, 0x80, 0x01, 0x98, 0xE1, 0x21, 0x00, 0x00, 0x46 },
    // 720p
    { 0x0C, 0x00, 0x48, 0x80, 0x80, 0x80, 0x01, 0x98, 0xE1, 0x21, 0x00, 0x00, 0x46 },
    // 1080i
    { 0x0C, 0x00, 0x48, 0x80, 0x80, 0x80, 0x01, 0x98, 0xE1, 0x21, 0x00, 0x00, 0x46 },
};

// Focus FS454/455 VID_CNTL0 (16-bit reg 0x92) values per mode
static const WORD s_focusVidCntl[5] =
{
    0x48C5,  // 480i
    0x483E,  // 480p
    0x48C5,  // 576i PAL  (same as 480i interlaced, different NV2A timing)
    0x582E,  // 720p
    0x50AE,  // 1080i
};

static void EncReprogramMode(int modeIdx)
{
    if (modeIdx < 0 || modeIdx >= ALL_MODE_COUNT) return;

    if (s_encType == ENC_CONEXANT)
    {
        const BYTE* vals = s_cnxtVals[modeIdx];
        for (int i = 0; i < 13; ++i)
            SMBusWrite(SMBADDR_ENC_CNXT, s_cnxtRegs[i], vals[i]);
    }
    else if (s_encType == ENC_FOCUS)
    {
        WORD val = s_focusVidCntl[modeIdx];
        SMBusWrite(SMBADDR_ENC_FOCUS, 0x92, (BYTE)(val & 0xFF));
        SMBusWrite(SMBADDR_ENC_FOCUS, 0x93, (BYTE)(val >> 8));
    }
    // ENC_XCALIBUR — no known register map, skip silently
}

// Switch D3D device to the mode at s_allModes[allIdx].
// Returns false if Reset() failed.
//
// Generic path  — A8R8G8B8, depth stencil on, DISCARD
// 1080i path    — X8R8G8B8, depth stencil off, COPY
// Both          — INTERVAL_IMMEDIATE (avoids blocking Present on unsupported modes)
static bool SwitchMode(int allIdx)
{
    if (allIdx < 0 || allIdx >= ALL_MODE_COUNT) return false;
    const ModeEntry& m = s_allModes[allIdx];
    bool is1080i = (m.width == 1920 && m.height == 1080);

    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = m.width;
    pp.BackBufferHeight = m.height;
    pp.BackBufferCount = 1;
    pp.Flags = m.presentFlags;
    pp.FullScreen_RefreshRateInHz = m.refreshHz;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    if (is1080i)
    {
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.EnableAutoDepthStencil = FALSE;
        pp.SwapEffect = D3DSWAPEFFECT_COPY;
    }
    else
    {
        pp.BackBufferFormat = D3DFMT_A8R8G8B8;
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    }

    s_resetOK = !FAILED(g_pDevice->Reset(&pp));
    if (!s_resetOK) return false;

    EncReprogramMode(allIdx);

    g_sx = (float)m.width / SW;
    g_sy = (float)m.height / SH;
    StrCopy(g_videoModeStr, sizeof(g_videoModeStr), m.label);
    g_isHD = (m.width > 800);

    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    s_settleFrames = 3;
    s_bbW = 0; s_bbH = 0;

    if (m.width != 640 || !(m.presentFlags & D3DPRESENTFLAG_INTERLACED))
        s_modeTimeout = GetTickCount() + MODE_TIMEOUT_MS;
    else
        s_modeTimeout = 0;

    return true;
}

// Restore D3D device to the original mode.
static void RestoreMode()
{
    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = s_origWidth;
    pp.BackBufferHeight = s_origHeight;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferCount = 1;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.Flags = s_origFlags;
    pp.FullScreen_RefreshRateInHz = s_origRefresh;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    g_pDevice->Reset(&pp);
    EncReprogramMode(0);

    g_sx = (float)s_origWidth / SW;
    g_sy = (float)s_origHeight / SH;
    g_isHD = (s_origWidth > 800);
    StrCopy(g_videoModeStr, sizeof(g_videoModeStr), s_origLabel);

    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

// ============================================================================
// DrawModeTest
// ============================================================================

static void DrawModeTest()
{
    // Which s_allModes index is currently active?
    int curAllIdx = -1;
    if (s_forceMode)
        curAllIdx = s_forceAllIdx;
    else if (s_modeIdx >= 0 && s_modeIdx < s_modeCount)
        curAllIdx = s_modeList[s_modeIdx];

    const ModeEntry& m = (curAllIdx >= 0) ? s_allModes[curAllIdx] : s_allModes[0];

    g_pDevice->BeginScene();
    DrawColorBarsContent(m.isPAL);

    // ---- Overlay -----------------------------------------------------------
    const float OVH = 136.f;
    const float OY = SH - OVH;

    FillRect(0.f, OY, SW, SH, D3DCOLOR_ARGB(220, 0, 0, 0));
    HLine(OY, 0.f, SW, COL_BORDER);

    char tbuf[16];

    // -- Row 0: mode label + resolution + refresh --
    static char s_info[56];
    StrCopy(s_info, sizeof(s_info), m.label);
    StrCat2(s_info, sizeof(s_info), s_info, "   ");
    IntToStr((int)m.width, tbuf, sizeof(tbuf));  StrCat2(s_info, sizeof(s_info), s_info, tbuf);
    StrCat2(s_info, sizeof(s_info), s_info, "x");
    IntToStr((int)m.height, tbuf, sizeof(tbuf)); StrCat2(s_info, sizeof(s_info), s_info, tbuf);
    StrCat2(s_info, sizeof(s_info), s_info, " @ ");
    IntToStr((int)m.refreshHz, tbuf, sizeof(tbuf)); StrCat2(s_info, sizeof(s_info), s_info, tbuf);
    StrCat2(s_info, sizeof(s_info), s_info, "Hz");
    DrawText(LM, OY + 5.f, s_info, 1.2f, s_forceMode ? COL_ORANGE : COL_CYAN);

    // Force-try badge
    if (s_forceMode)
        DrawText(LM + TW(s_info, 1.2f) + 8.f, OY + 5.f, "[FORCED]", 1.0f, COL_ORANGE);

    // -- Row 1: flags --
    static char s_flags[48];
    StrCopy(s_flags, sizeof(s_flags),
        (m.presentFlags & D3DPRESENTFLAG_PROGRESSIVE) ? "PROGRESSIVE" : "INTERLACED");
    if (m.presentFlags & D3DPRESENTFLAG_WIDESCREEN)
        StrCat2(s_flags, sizeof(s_flags), s_flags, "  WIDESCREEN");
    if (m.isPAL)
        StrCat2(s_flags, sizeof(s_flags), s_flags, "  PAL");
    DrawText(LM, OY + 21.f, s_flags, 1.0f, D3DCOLOR_XRGB(140, 200, 140));

    // -- Row 2: separator + AV pack / EEPROM context --
    HLine(OY + 34.f, 0.f, SW, D3DCOLOR_XRGB(40, 40, 55));

    static char s_avLine[56];
    StrCopy(s_avLine, sizeof(s_avLine), "AV: ");
    StrCat2(s_avLine, sizeof(s_avLine), s_avLine, AvPackName(s_avPackTest));
    StrCat2(s_avLine, sizeof(s_avLine), s_avLine, "   EEPROM FLAGS: ");
    if (s_eepFlagsOK)
    {
        StrCat2(s_avLine, sizeof(s_avLine), s_avLine,
            (s_eepVideoFlags & XC_VIDEO_FLAGS_HDTV_480p) ? "480p " : "");
        StrCat2(s_avLine, sizeof(s_avLine), s_avLine,
            (s_eepVideoFlags & XC_VIDEO_FLAGS_HDTV_720p) ? "720p " : "");
        StrCat2(s_avLine, sizeof(s_avLine), s_avLine,
            (s_eepVideoFlags & XC_VIDEO_FLAGS_HDTV_1080i) ? "1080i" : "");
        // If none set
        bool anyHD = (s_eepVideoFlags & (XC_VIDEO_FLAGS_HDTV_480p | XC_VIDEO_FLAGS_HDTV_720p | XC_VIDEO_FLAGS_HDTV_1080i)) != 0;
        if (!anyHD) StrCat2(s_avLine, sizeof(s_avLine), s_avLine, "none");
    }
    else
        StrCat2(s_avLine, sizeof(s_avLine), s_avLine, "unknown");
    DrawText(LM, OY + 38.f, s_avLine, 1.0f, D3DCOLOR_XRGB(100, 100, 120));

    // Encoder label — right side of context row
    {
        const char* encLabel =
            (s_encType == ENC_CONEXANT) ? "ENC: CONEXANT" :
            (s_encType == ENC_FOCUS) ? "ENC: FOCUS" :
            (s_encType == ENC_XCALIBUR) ? "ENC: XCALIBUR" : "ENC: UNKNOWN";
        DWORD encCol =
            (s_encType == ENC_XCALIBUR) ? D3DCOLOR_XRGB(160, 120, 40) :
            D3DCOLOR_XRGB(100, 140, 100);
        DrawText(SW - LM - TW(encLabel, 1.0f), OY + 38.f, encLabel, 1.0f, encCol);
    }

    // -- Mode list --
    const float LIST_Y0 = OY + 52.f;
    const float LIST_DY = 12.f;
    const float COL_LABEL_X = LM + 12.f;
    const float COL_RES_X = LM + 80.f;
    const float COL_HZ_X = LM + 168.f;
    const float COL_FLAGS_X = LM + 210.f;
    const float COL_NOTE_X = LM + 292.f;

    for (int ai = 0; ai < ALL_MODE_COUNT; ++ai)
    {
        const ModeEntry& e = s_allModes[ai];
        const ModeProbe& pr = s_probes[ai];
        bool isCurrent = (ai == curAllIdx);
        float ry = LIST_Y0 + (float)ai * LIST_DY;

        if (isCurrent)
            DrawText(LM, ry, ">", 1.0f, s_forceMode ? COL_ORANGE : COL_CYAN);

        // Color: current=bright, supported=mid, unknown/experimental=amber, blocked=dim
        DWORD labelCol = isCurrent
            ? (s_forceMode ? COL_ORANGE : COL_WHITE)
            : (pr.status == PROBE_SUPPORTED)
            ? D3DCOLOR_XRGB(160, 160, 160)
            : (pr.status == PROBE_UNKNOWN)
            ? D3DCOLOR_XRGB(180, 140, 60)   // amber — selectable but experimental
            : D3DCOLOR_XRGB(55, 55, 65);    // dim — hard blocked

        DrawText(COL_LABEL_X, ry, e.label, 1.0f, labelCol);

        static char s_res[20];
        IntToStr((int)e.width, tbuf, sizeof(tbuf));  StrCopy(s_res, sizeof(s_res), tbuf);
        StrCat2(s_res, sizeof(s_res), s_res, "x");
        IntToStr((int)e.height, tbuf, sizeof(tbuf)); StrCat2(s_res, sizeof(s_res), s_res, tbuf);
        DrawText(COL_RES_X, ry, s_res, 1.0f, labelCol);

        static char s_hz[10];
        IntToStr((int)e.refreshHz, tbuf, sizeof(tbuf));
        StrCopy(s_hz, sizeof(s_hz), tbuf);
        StrCat2(s_hz, sizeof(s_hz), s_hz, "Hz");
        DrawText(COL_HZ_X, ry, s_hz, 1.0f, labelCol);

        if (e.presentFlags & D3DPRESENTFLAG_WIDESCREEN)
            DrawText(COL_FLAGS_X, ry, "WIDE", 1.0f, labelCol);
        if (e.isPAL)
            DrawText(COL_FLAGS_X + (e.presentFlags & D3DPRESENTFLAG_WIDESCREEN ? 36.f : 0.f),
                ry, "PAL", 1.0f, labelCol);

        // Reason column
        if (pr.status == PROBE_UNKNOWN)
        {
            // Experimental — selectable but warn
            DrawText(COL_NOTE_X, ry, "EXPERIMENTAL", 1.0f,
                isCurrent ? COL_ORANGE : D3DCOLOR_XRGB(140, 100, 40));
        }
        else if (pr.status != PROBE_SUPPORTED)
        {
            static char s_note[40];
            StrCopy(s_note, sizeof(s_note), "BLOCKED: ");
            StrCat2(s_note, sizeof(s_note), s_note, pr.reason);
            DrawText(COL_NOTE_X, ry, s_note, 1.0f, D3DCOLOR_XRGB(130, 60, 60));
        }
        else
        {
            // Show display requirement for modes needing specific TV support
            const char* req = NULL;
            if (e.width == 640 && (e.presentFlags & D3DPRESENTFLAG_PROGRESSIVE))
                req = "480p TV";
            else if (e.width == 1280)
                req = "720p TV";
            else if (e.width == 1920)
                req = "1080i TV";
            if (req)
                DrawText(COL_NOTE_X, ry, req, 1.0f,
                    isCurrent ? D3DCOLOR_XRGB(180, 180, 100) : D3DCOLOR_XRGB(90, 90, 70));
        }
    }

    // -- BB verify --
    {
        float verY = OY + OVH - 26.f;
        if (s_settleFrames > 0)
        {
            DrawText(SW - LM - TW("BB: settling...", 1.0f), verY,
                "BB: settling...", 1.0f, COL_DIM);
            --s_settleFrames;
        }
        else
        {
            // Read backbuffer once per frame after settle
            if (s_bbW == 0)
            {
                LPDIRECT3DSURFACE8 pBB = NULL;
                if (SUCCEEDED(g_pDevice->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &pBB)) && pBB)
                {
                    D3DSURFACE_DESC desc;
                    if (SUCCEEDED(pBB->GetDesc(&desc))) { s_bbW = desc.Width; s_bbH = desc.Height; }
                    pBB->Release();
                }
            }
            bool match = (s_bbW == m.width && s_bbH == m.height);
            static char s_bb[32];
            StrCopy(s_bb, sizeof(s_bb), "BB: ");
            if (s_bbW > 0)
            {
                char wa[8], ha[8];
                IntToStr((int)s_bbW, wa, sizeof(wa));
                IntToStr((int)s_bbH, ha, sizeof(ha));
                StrCat2(s_bb, sizeof(s_bb), s_bb, wa);
                StrCat2(s_bb, sizeof(s_bb), s_bb, "x");
                StrCat2(s_bb, sizeof(s_bb), s_bb, ha);
                StrCat2(s_bb, sizeof(s_bb), s_bb, match ? "  MATCH" : "  MISMATCH");
            }
            else StrCat2(s_bb, sizeof(s_bb), s_bb, "query failed");
            DrawText(SW - LM - TW(s_bb, 1.0f), verY, s_bb, 1.0f,
                (s_bbW == 0) ? COL_RED : (match ? COL_GREEN : COL_ORANGE));
        }
    }

    // -- Navigation hint + auto-restore countdown --
    if (s_modeTimeout != 0)
    {
        DWORD now = GetTickCount();
        int secsLeft = (s_modeTimeout > now) ? (int)((s_modeTimeout - now) / 1000 + 1) : 0;
        static char s_hint[64];
        StrCopy(s_hint, sizeof(s_hint), "[B] Restore now    Auto-restore in ");
        char sc[4]; IntToStr(secsLeft, sc, sizeof(sc));
        StrCat2(s_hint, sizeof(s_hint), s_hint, sc);
        StrCat2(s_hint, sizeof(s_hint), s_hint, "s...");
        DrawText(LM, OY + OVH - 13.f, s_hint, 1.0f, COL_ORANGE);
    }
    else
    {
        DrawText(LM, OY + OVH - 13.f,
            "[WHITE] Next    [BLACK] Prev    [A] Force blocked    [B] Restore & Exit",
            1.0f, COL_YELLOW);
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// DrawColorBarsContent — pure drawing, no scene management.
// Called by both DrawColorBars (standalone) and DrawModeTest (embedded).
// ============================================================================

static void DrawColorBarsContent(bool isPAL)
{
    FillRect(0.f, 0.f, SW, SH, D3DCOLOR_XRGB(0, 0, 0));

    if (!isPAL)
    {
        const float topH = SH * 0.667f;
        const float midH = SH * 0.750f;
        const float barW = SW / 7.f;

        static const DWORD ntscTop[7] =
        {
            D3DCOLOR_XRGB(191, 191, 191),
            D3DCOLOR_XRGB(191, 191,   0),
            D3DCOLOR_XRGB(0, 191, 191),
            D3DCOLOR_XRGB(0, 191,   0),
            D3DCOLOR_XRGB(191,   0, 191),
            D3DCOLOR_XRGB(191,   0,   0),
            D3DCOLOR_XRGB(0,   0, 191),
        };
        for (int i = 0; i < 7; ++i)
        {
            float x0 = barW * (float)i;
            float x1 = barW * (float)(i + 1);
            FillRect(x0, 0.f, x1, topH, ntscTop[i]);
        }

        static const DWORD ntscMid[7] =
        {
            D3DCOLOR_XRGB(0,   0, 191),
            D3DCOLOR_XRGB(0,   0,   0),
            D3DCOLOR_XRGB(191,   0, 191),
            D3DCOLOR_XRGB(0,   0,   0),
            D3DCOLOR_XRGB(0, 191, 191),
            D3DCOLOR_XRGB(0,   0,   0),
            D3DCOLOR_XRGB(191, 191, 191),
        };
        for (int i = 0; i < 7; ++i)
        {
            float x0 = barW * (float)i;
            float x1 = barW * (float)(i + 1);
            FillRect(x0, topH, x1, midH, ntscMid[i]);
        }

        static const DWORD ntscBot[7] =
        {
            D3DCOLOR_XRGB(0,  33,  76),
            D3DCOLOR_XRGB(255, 255, 255),
            D3DCOLOR_XRGB(50,   0,  71),
            D3DCOLOR_XRGB(7,   7,   7),
            D3DCOLOR_XRGB(0,   0,   0),
            D3DCOLOR_XRGB(18,  18,  18),
            D3DCOLOR_XRGB(0,   0,   0),
        };
        for (int i = 0; i < 7; ++i)
        {
            float x0 = barW * (float)i;
            float x1 = barW * (float)(i + 1);
            FillRect(x0, midH, x1, SH, ntscBot[i]);
        }
    }
    else
    {
        const float topH = SH * 0.75f;
        const float barW = SW / 8.f;

        static const DWORD palTop[8] =
        {
            D3DCOLOR_XRGB(255, 255, 255),
            D3DCOLOR_XRGB(255, 255,   0),
            D3DCOLOR_XRGB(0, 255, 255),
            D3DCOLOR_XRGB(0, 255,   0),
            D3DCOLOR_XRGB(255,   0, 255),
            D3DCOLOR_XRGB(255,   0,   0),
            D3DCOLOR_XRGB(0,   0, 255),
            D3DCOLOR_XRGB(0,   0,   0),
        };
        for (int i = 0; i < 8; ++i)
        {
            float x0 = barW * (float)i;
            float x1 = barW * (float)(i + 1);
            FillRect(x0, 0.f, x1, topH, palTop[i]);
        }

        static const DWORD palBot[8] =
        {
            D3DCOLOR_XRGB(104, 104, 104),
            D3DCOLOR_XRGB(0,   0,   0),
            D3DCOLOR_XRGB(104, 104, 104),
            D3DCOLOR_XRGB(0,   0,   0),
            D3DCOLOR_XRGB(104, 104, 104),
            D3DCOLOR_XRGB(0,   0,   0),
            D3DCOLOR_XRGB(104, 104, 104),
            D3DCOLOR_XRGB(0,   0,   0),
        };
        for (int i = 0; i < 8; ++i)
        {
            float x0 = barW * (float)i;
            float x1 = barW * (float)(i + 1);
            FillRect(x0, topH, x1, SH, palBot[i]);
        }
    }

    // Exit hint (only shown in standalone bar modes, visible below bars)
    DrawText(6.f, SH - 14.f, "[B] Back", 1.1f, D3DCOLOR_ARGB(160, 255, 255, 255));
}

// Standalone color bar frame — used by VSS_NTSC_BARS and VSS_PAL_BARS.
static void DrawColorBars(bool isPAL)
{
    g_pDevice->BeginScene();
    DrawColorBarsContent(isPAL);
    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

void VideoInfo_OnEnter()
{
    s_prevBtns = 0;
    s_loaded = false;
    s_subState = VSS_INFO;
    s_modeTimeout = 0;
    s_forceMode = false;
    s_forceAllIdx = -1;
    s_bbW = 0; s_bbH = 0;
    s_settleFrames = 0;
    // ---- Capture original presentation parameters for mode test restoration ----
    // Read backbuffer dimensions from D3D; infer present flags by matching
    // width+height against s_allModes[] (avoids exporting VideoMode from main.cpp).
    s_origWidth = 640;
    s_origHeight = 480;
    s_origRefresh = 60;
    s_origFlags = D3DPRESENTFLAG_INTERLACED;  // safe default

    if (g_pDevice)
    {
        // Backbuffer size
        LPDIRECT3DSURFACE8 pBB = NULL;
        if (SUCCEEDED(g_pDevice->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &pBB)) && pBB)
        {
            D3DSURFACE_DESC desc;
            if (SUCCEEDED(pBB->GetDesc(&desc)))
            {
                s_origWidth = desc.Width;
                s_origHeight = desc.Height;
            }
            pBB->Release();
        }
        // Refresh rate
        D3DDISPLAYMODE dm;
        ZeroMemory(&dm, sizeof(dm));
        if (SUCCEEDED(g_pDevice->GetDisplayMode(&dm)) && dm.RefreshRate > 0)
            s_origRefresh = dm.RefreshRate;
    }

    // Capture the original mode label first — g_videoModeStr is the only
    // unambiguous source since 480i and 480p are both 640x480 at 60Hz.
    StrCopy(s_origLabel, sizeof(s_origLabel), g_videoModeStr);

    // Look up the rest of the parameters from the mode table.
    for (int i = 0; i < ALL_MODE_COUNT; ++i)
    {
        const char* a = s_allModes[i].label;
        const char* b = s_origLabel;
        int j = 0;
        while (a[j] && b[j] && a[j] == b[j]) ++j;
        if (!a[j] && !b[j])
        {
            s_origFlags = s_allModes[i].presentFlags;
            s_origWidth = s_allModes[i].width;
            s_origHeight = s_allModes[i].height;
            s_origRefresh = s_allModes[i].refreshHz;
            break;
        }
    }

    // ---- Probe AV pack and EEPROM video flags — both needed for BuildProbeTable ----
    s_avPackTest = 0xFF;
    {
        BYTE av = 0;
        if (SMBusRead(SMBADDR_PIC, 0x04, av))
            s_avPackTest = av & 0x07;
    }

    s_eepVideoFlags = 0;
    s_eepFlagsOK = false;
    {
        ULONG type = 0, len = 0;
        if (ExQueryNonVolatileSetting(XC_VIDEO_FLAGS, &type, &s_eepVideoFlags, 4, &len) == 0)
            s_eepFlagsOK = true;
    }

    // Build the preflight probe table now — no switching, pure policy classification
    BuildProbeTable();
    s_modeIdx = 0;
    s_forceMode = false;
    s_forceAllIdx = -1;
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

static void Render(const DiagLogo& logo)
{
    g_pDevice->BeginScene();
    DrawPageChrome(logo, "VIDEO INFO", "[X] NTSC Bars  [Y] PAL Bars  [WHITE] Mode Test  [B] Back");

    const VideoData& d = s_data;

    VLine(COL_R - 10.f, CY, BOT_BAR_Y - 4.f, COL_BORDER);

    float y1 = CY;
    float y2 = CY;

    // ---- LEFT: DISPLAY ----
    DrawText(COL_L, y1, "DISPLAY", 1.3f, COL_YELLOW);
    HLine(y1 + LH + 1.f, COL_L, COL_R - 12.f, COL_BORDER);
    y1 += LH + 6.f;

    DrawRow(COL_L, COL_VL, y1, "MODE    :", d.modeStr,
        g_isHD ? COL_CYAN : COL_WHITE);                    y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "RES     :", d.resStr,
        COL_CYAN);                                         y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "REFRESH :", d.refreshStr);    y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "HD MODE :", d.isHDStr,
        g_isHD ? COL_GREEN : COL_GRAY);                   y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "SCALE X :", d.scaleX);        y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "SCALE Y :", d.scaleY);        y1 += LH + GAP;

    // ---- LEFT: FRAMEBUFFER ----
    DrawText(COL_L, y1, "FRAMEBUFFER", 1.3f, COL_YELLOW);
    HLine(y1 + LH + 1.f, COL_L, COL_R - 12.f, COL_BORDER);
    y1 += LH + 6.f;

    if (d.bbWidth && d.bbHeight)
    {
        char t[8];
        IntToStr(d.bbWidth, t, sizeof(t));
        DrawRow(COL_L, COL_VL, y1, "WIDTH   :", t);           y1 += LH;
        IntToStr(d.bbHeight, t, sizeof(t));
        DrawRow(COL_L, COL_VL, y1, "HEIGHT  :", t);           y1 += LH;
    }
    else
    {
        DrawRow(COL_L, COL_VL, y1, "SIZE    :", d.resStr);    y1 += LH;
        y1 += LH;
    }
    y1 += GAP;

    // ---- LEFT: NV2A ----
    DrawText(COL_L, y1, "NV2A", 1.3f, COL_YELLOW);
    HLine(y1 + LH + 1.f, COL_L, COL_R - 12.f, COL_BORDER);
    y1 += LH + 6.f;

    DrawRow(COL_L, COL_VL, y1, "GPU CLK :", d.gpuClkStr,
        COL_GREEN);                                           y1 += LH;
    DrawText(COL_VL, y1, d.nvpllRaw, 0.95f, COL_DIM);      y1 += LH - 4.f;
    DrawRow(COL_L, COL_VL, y1, "MEM CLK :", d.memClkStr,
        COL_CYAN);                                            y1 += LH;
    // Raw MPLL — (est) means MPLL x2 assumed DDR; known values (200/256) shown without qualifier
    DrawText(COL_VL, y1, d.mpllRaw, 0.95f, COL_DIM);        y1 += LH - 4.f;
    DrawRow(COL_L, COL_VL, y1, "PIX CLK :", d.pixClkStr);   y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "FB BASE :", d.fbBaseStr,
        COL_CYAN);                                            y1 += LH;

    // ---- RIGHT: ENCODER ----
    DrawText(COL_R, y2, "ENCODER", 1.3f, COL_YELLOW);
    HLine(y2 + LH + 1.f, COL_R, SW - LM, COL_BORDER);
    y2 += LH + 6.f;

    DrawRow(COL_R, COL_VR, y2, "CHIP    :", d.encName,
        d.encAck ? COL_CYAN : COL_YELLOW);                 y2 += LH;
    DrawRow(COL_R, COL_VR, y2, "CHIP ID :", d.encIdStr,
        d.encAck ? COL_WHITE : COL_GRAY);                  y2 += LH;
    {
        DWORD stdCol = COL_GRAY;
        if (d.encAck)
            stdCol = (d.encStd[0] == 'N') ? COL_WHITE : COL_YELLOW; // NTSC=white, PAL=yellow
        DrawRow(COL_R, COL_VR, y2, "STANDARD:", d.encStd, stdCol);  y2 += LH;
    }

    // Encoder capabilities / outputs row
    {
        const char* caps = NULL;
        if (d.encAck)
        {
            switch (d.encIdByte)
            {
            case 0x76: caps = "Composite / S-Video / SCART";      break;
            case 0x54: caps = "Component / 480p / 1080i";          break;
            case 0x09: caps = "Component / 480p / 720p";           break;
            }
        }
        else
        {
            // Xcalibur (1.6) - integrated NV2A encoder, no SMBus ID
            caps = "VGA / Component / Composite";
        }
        if (caps)
        {
            DrawText(COL_R, y2, "OUTPUTS :", 1.2f, COL_GRAY);
            DrawText(COL_VR, y2, caps, 1.2f, COL_WHITE);
        }
    }
    y2 += LH + GAP;

    // ---- RIGHT: AV PACK ----
    DrawText(COL_R, y2, "AV PACK", 1.3f, COL_YELLOW);
    HLine(y2 + LH + 1.f, COL_R, SW - LM, COL_BORDER);
    y2 += LH + 6.f;

    // AV pack with color coding
    DWORD avCol = COL_WHITE;
    if (StrLen(d.avPack) == 0)                              avCol = COL_GRAY;
    else if (d.avPack[0] == 'H')                                 avCol = COL_CYAN;  // HDTV
    else if (d.avPack[0] == 'V')                                 avCol = COL_GREEN; // VGA
    else if (d.avPack[0] == 'C' && d.avPack[1] == 'o')          avCol = COL_WHITE; // Composite
    else if (d.avPack[0] == 'S')                                 avCol = COL_WHITE; // S-Video
    else if (d.avPack[0] == 'N')                                 avCol = COL_GRAY;  // None
    else if (d.avPack[0] == 'P')                                 avCol = COL_RED;   // PIC NAK

    DrawRow(COL_R, COL_VR, y2, "TYPE    :", d.avPack, avCol);   y2 += LH + GAP;

    // ---- RIGHT: XEMU NOTE ----
    // On xemu the AV pack PIC may NAK — add a soft note
    if (d.avPack[0] == 'P')
    {
        DrawText(COL_R, y2, "PIC (0x20) not responding.", 1.1f, COL_DIM);
        y2 += LH;
        DrawText(COL_R, y2, "Normal on xemu.", 1.1f, COL_DIM);
        y2 += LH;
    }

    // ---- RIGHT: HD mod ----
    if (d.hdModVer[0] != '\0')
    {
        y2 += GAP;
        DrawText(COL_R, y2, "HD MOD", 1.3f, COL_YELLOW);
        HLine(y2 + LH + 1.f, COL_R, SW - LM, COL_BORDER);
        y2 += LH + 6.f;
        DrawRow(COL_R, COL_VR, y2, "VERSION :", d.hdModVer, COL_CYAN);  y2 += LH;
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// Tick
// ============================================================================

void VideoInfo_Tick(const DiagLogo& logo)
{
    if (!s_loaded)
    {
        // Render a loading frame first, then do the blocking SMBus reads
        g_pDevice->BeginScene();
        DrawPageChrome(logo, "VIDEO INFO", "[B] Back");
        DrawText(LM, CONTENT_Y + 20.f, "Reading hardware...", 1.4f, COL_YELLOW);
        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
        LoadData();
        // Re-sync avPackTest and detect encoder type from confirmed LoadData reads.
        {
            BYTE av = 0;
            if (SMBusRead(SMBADDR_PIC, 0x04, av))
                s_avPackTest = av & 0x07;
        }
        // Encoder detection — needed so mode test UI can warn about Xcalibur limits
        {
            BYTE pid = 0;
            if (SMBusRead(SMBADDR_ENC_CNXT, 0x00, pid)) s_encType = ENC_CONEXANT;
            else if (SMBusRead(SMBADDR_ENC_FOCUS, 0x00, pid)) s_encType = ENC_FOCUS;
            else                                                s_encType = ENC_XCALIBUR;
        }
        BuildProbeTable();
        s_loaded = true;
        return;
    }

    WORD cur = GetButtons();

    // ---- VSS_NTSC_BARS / VSS_PAL_BARS ----
    if (s_subState == VSS_NTSC_BARS || s_subState == VSS_PAL_BARS)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
        {
            s_subState = VSS_INFO;
            s_prevBtns = cur;
            return;
        }
        s_prevBtns = cur;
        DrawColorBars(s_subState == VSS_PAL_BARS);
        return;
    }

    // ---- VSS_MODE_TEST ----
    if (s_subState == VSS_MODE_TEST)
    {
        // Auto-restore: if timeout has expired, restore 480i automatically.
        // This recovers from black screen when the TV can't display the mode.
        if (s_modeTimeout != 0 && GetTickCount() >= s_modeTimeout)
        {
            s_modeTimeout = 0;
            s_forceMode = false;
            s_forceAllIdx = -1;
            // Switch back to 480i (index 0 in s_allModes)
            s_modeIdx = 0;
            for (int i = 0; i < s_modeCount; ++i)
                if (s_modeList[i] == 0) { s_modeIdx = i; break; }
            SwitchMode(s_modeList[s_modeIdx]);
            s_prevBtns = cur;
            DrawModeTest();
            return;
        }

        if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
        {
            s_modeTimeout = 0;
            RestoreMode();
            s_subState = VSS_INFO;
            s_loaded = false;
            s_forceMode = false;
            s_forceAllIdx = -1;
            g_pDevice->BeginScene();
            g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.f, 0);
            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);
            s_prevBtns = cur;
            return;
        }

        // [WHITE] — next supported mode
        if (EdgeDown(cur, s_prevBtns, BTN_WHITE))
        {
            s_forceMode = false;
            s_forceAllIdx = -1;
            if (s_modeCount > 0)
            {
                s_modeIdx = (s_modeIdx + 1) % s_modeCount;
                SwitchMode(s_modeList[s_modeIdx]);
            }
            s_prevBtns = cur;
            DrawModeTest();
            return;
        }

        // [BLACK] — previous supported mode
        if (EdgeDown(cur, s_prevBtns, BTN_BLACK))
        {
            s_forceMode = false;
            s_forceAllIdx = -1;
            if (s_modeCount > 0)
            {
                s_modeIdx = (s_modeIdx - 1 + s_modeCount) % s_modeCount;
                SwitchMode(s_modeList[s_modeIdx]);
            }
            s_prevBtns = cur;
            DrawModeTest();
            return;
        }

        // [A] — force-try next blocked mode
        if (EdgeDown(cur, s_prevBtns, BTN_A))
        {
            // Find the next blocked mode after the current forced index (or from 0)
            int startSearch = (s_forceAllIdx >= 0) ? (s_forceAllIdx + 1) % ALL_MODE_COUNT : 0;
            int nextBlocked = -1;
            for (int i = 0; i < ALL_MODE_COUNT; ++i)
            {
                int ai = (startSearch + i) % ALL_MODE_COUNT;
                if (s_probes[ai].status != PROBE_SUPPORTED)
                {
                    nextBlocked = ai;
                    break;
                }
            }
            if (nextBlocked >= 0)
            {
                s_forceMode = true;
                s_forceAllIdx = nextBlocked;
                SwitchMode(nextBlocked);
            }
            s_prevBtns = cur;
            DrawModeTest();
            return;
        }

        s_prevBtns = cur;
        DrawModeTest();
        return;
    }

    // ---- VSS_INFO ----
    if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
    {
        RequestState(MSTATE_MENU);
        s_prevBtns = cur;
        return;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_X))
    {
        s_subState = VSS_NTSC_BARS;
        s_prevBtns = cur;
        return;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_Y))
    {
        s_subState = VSS_PAL_BARS;
        s_prevBtns = cur;
        return;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_WHITE))
    {
        // Probe table was built at OnEnter — just enter mode test and switch to first supported mode
        s_modeIdx = 0;
        s_forceMode = false;
        s_forceAllIdx = -1;
        s_settleFrames = 0;
        s_bbW = 0; s_bbH = 0;
        s_subState = VSS_MODE_TEST;
        if (s_modeCount > 0)
            SwitchMode(s_modeList[0]);
        s_prevBtns = cur;
        return;
    }

    s_prevBtns = cur;
    Render(logo);
}
// ============================================================================
// AutoRun — headless data gather for XbSet automation
// ============================================================================

// Helper: write label+value line to report handle
static void VIWriteLine(HANDLE hf, const char* label, const char* val)
{
    char line[128]; DWORD w;
    StrCopy(line, sizeof(line), label);
    StrCat2(line, sizeof(line), line, val);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
}

void VideoInfo_AutoRun(HANDLE hReport)
{
    LoadData();
    s_loaded = true;

    const VideoData& d = s_data;
    VIWriteLine(hReport, "Mode:       ", d.modeStr);
    VIWriteLine(hReport, "Resolution: ", d.resStr);
    VIWriteLine(hReport, "Refresh:    ", d.refreshStr);
    VIWriteLine(hReport, "HD Mode:    ", d.isHDStr);
    VIWriteLine(hReport, "GPU CLK:    ", d.gpuClkStr);
    VIWriteLine(hReport, "Mem CLK:    ", d.memClkStr);
    VIWriteLine(hReport, "VRAM:       ", d.vramStr);
    VIWriteLine(hReport, "Encoder:    ", d.encName);
    VIWriteLine(hReport, "AV Pack:    ", d.avPack);
    VIWriteLine(hReport, "NVPLL Raw:  ", d.nvpllRaw);
}