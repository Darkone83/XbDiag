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
//  [BOT BAR]  [B] Back
//
// Sources:
//   Video mode / resolution  - g_videoModeStr, g_sx, g_sy, backbuffer desc
//   Encoder chip             - SMBus 0x8A (Conexant) / 0xD4 (Focus) reg 0x00 (ID byte)
//   AV pack                  - SMBus 0x20 (PIC16L) reg 0x04 bits [2:0]

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
static const float COL_VR = COL_R + 110.f;
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

    // Encoder (SMBus 0x10 reg 0x00)
    bool encAck;
    BYTE encIdByte;
    char encName[28];    // "Conexant CX25871" etc
    char encIdStr[8];    // "0x76" etc

    // Encoder output standard (from encoder reg 0x02 / 0x01)
    char encStd[16];     // "NTSC" / "PAL-B/G" / "PAL-M" / "PAL-N" / "N/A"

    // AV pack (SMBus 0x45 reg 0x04)
    char avPack[24];     // "Composite" / "HDTV" / "S-Video" etc

    // NV2A registers (MMIO 0xFD000000)
    bool nv2aOK;
    char memClkStr[12];  // "200 MHz"
    char pixClkStr[12];  // "74 MHz"
    char fbBaseStr[12];  // "0x00FC0000"
    char vramStr[8];     // "64 MB" / "128 MB"
};

static VideoData s_data;
static WORD      s_prevBtns;
static bool      s_loaded = false;

enum VideoSubState { VSS_INFO, VSS_NTSC_BARS, VSS_PAL_BARS };
static VideoSubState s_subState = VSS_INFO;

// ============================================================================
// Helpers
// ============================================================================

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

// Simple float to "X.XX" string (positive values only, 2 decimal places)
static void FmtFloat2(float v, char* out, int outLen)
{
    int whole = Ftoi(v);
    int frac = Ftoi((v - (float)whole) * 100.f + 0.5f);
    if (frac >= 100) { whole += 1; frac -= 100; }

    // Build directly into out: write whole digits, '.', frac digits
    int i = 0;
    // whole part (simple itoa, no leading zeros needed)
    char wbuf[8]; int wi = 0;
    int tmp = whole;
    if (tmp == 0) { wbuf[wi++] = '0'; }
    else { while (tmp > 0 && wi < 7) { wbuf[wi++] = '0' + (tmp % 10); tmp /= 10; } }
    // wbuf is reverse order
    for (int k = wi - 1; k >= 0 && i < outLen - 1; --k) out[i++] = wbuf[k];
    // decimal point
    if (i < outLen - 1) out[i++] = '.';
    // frac, always 2 digits
    if (i < outLen - 1) out[i++] = '0' + (frac / 10);
    if (i < outLen - 1) out[i++] = '0' + (frac % 10);
    out[i] = '\0';
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

// Decode NV2A PLL register (M/N/P) to MHz
// F_out = (16666 KHz * N) / (M * 2^P)
static DWORD NvPllToMHz(DWORD reg)
{
    DWORD M = (reg >> 0) & 0xFF;
    DWORD N = (reg >> 8) & 0xFF;
    DWORD P = (reg >> 16) & 0x0F;
    if (M == 0 || N == 0) return 0;
    DWORD fKHz = (16666UL * N) / (M * (1UL << P));
    return fKHz / 1000;
}

static void LoadNV2A(VideoData& d)
{
    d.nv2aOK = false;
    StrCopy(d.memClkStr, sizeof(d.memClkStr), "N/A");
    StrCopy(d.pixClkStr, sizeof(d.pixClkStr), "N/A");
    StrCopy(d.fbBaseStr, sizeof(d.fbBaseStr), "N/A");
    StrCopy(d.vramStr, sizeof(d.vramStr), "N/A");

    // Guard: confirm NV2A vendor via PCI before touching MMIO
    // NV2A: PCI vendor 0x10DE, device 0x02A0
    DWORD pciId = ViPciRead32(0, 0, 0, 0x00);
    if ((pciId & 0xFFFF) != 0x10DE)
        return;

    d.nv2aOK = true;

    // MPLL (memory clock) at PRAMDAC 0x680504
    {
        volatile DWORD* pMpll = (volatile DWORD*)(NV2A_BASE + 0x680504UL);
        DWORD mhz = NvPllToMHz(*pMpll);
        if (mhz > 0 && mhz < 600)
        {
            char t[8];
            IntToStr((int)mhz, t, sizeof(t));
            StrCat2(d.memClkStr, sizeof(d.memClkStr), t, " MHz");
        }
    }

    // VPLL1 (pixel clock) at PRAMDAC 0x680508
    {
        volatile DWORD* pVpll = (volatile DWORD*)(NV2A_BASE + 0x680508UL);
        DWORD mhz = NvPllToMHz(*pVpll);
        if (mhz > 0 && mhz < 600)
        {
            char t[8];
            IntToStr((int)mhz, t, sizeof(t));
            StrCat2(d.pixClkStr, sizeof(d.pixClkStr), t, " MHz");
        }
    }

    // PCRTC_START (framebuffer scanout base) at 0x600810
    {
        volatile DWORD* pFbBase = (volatile DWORD*)(NV2A_BASE + 0x600810UL);
        DWORD addr = *pFbBase;
        char t[10];
        IntToHex(addr, 8, t, sizeof(t));
        StrCat2(d.fbBaseStr, sizeof(d.fbBaseStr), "0x", t);
    }

    // PFB_BOOT_0 (VRAM size strap) at 0x100200, bit 2: 0=64MB 1=128MB
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

static void DrawColorBars(bool isPAL)
{
    g_pDevice->BeginScene();

    // Full black background first
    FillRect(0.f, 0.f, SW, SH, D3DCOLOR_XRGB(0, 0, 0));

    if (!isPAL)
    {
        // ----------------------------------------------------------------
        // NTSC SMPTE 75% color bars
        // ----------------------------------------------------------------
        // Top region: y = 0 .. topH
        // Mid stripe:  topH .. midH
        // Bot region:  midH .. SH

        const float topH = SH * 0.667f;   // 2/3 height
        const float midH = SH * 0.750f;   // thin stripe bottom
        const float barW = SW / 7.f;

        // 75% SMPTE colors (ITU-R BT.601 75% bars)
        static const DWORD ntscTop[7] =
        {
            D3DCOLOR_XRGB(191, 191, 191), // 75% White
            D3DCOLOR_XRGB(191, 191,   0), // Yellow
            D3DCOLOR_XRGB(0, 191, 191), // Cyan
            D3DCOLOR_XRGB(0, 191,   0), // Green
            D3DCOLOR_XRGB(191,   0, 191), // Magenta
            D3DCOLOR_XRGB(191,   0,   0), // Red
            D3DCOLOR_XRGB(0,   0, 191), // Blue
        };

        for (int i = 0; i < 7; ++i)
        {
            float x0 = barW * (float)i;
            float x1 = barW * (float)(i + 1);
            FillRect(x0, 0.f, x1, topH, ntscTop[i]);
        }

        // Mid stripe: reverse blue bars (Blue / Black / Magenta / Black /
        //             Cyan / Black / White) then PLUGE at far right
        static const DWORD ntscMid[7] =
        {
            D3DCOLOR_XRGB(0,   0, 191), // Blue
            D3DCOLOR_XRGB(0,   0,   0), // Black
            D3DCOLOR_XRGB(191,   0, 191), // Magenta
            D3DCOLOR_XRGB(0,   0,   0), // Black
            D3DCOLOR_XRGB(0, 191, 191), // Cyan
            D3DCOLOR_XRGB(0,   0,   0), // Black
            D3DCOLOR_XRGB(191, 191, 191), // 75% White
        };
        for (int i = 0; i < 7; ++i)
        {
            float x0 = barW * (float)i;
            float x1 = barW * (float)(i + 1);
            FillRect(x0, topH, x1, midH, ntscMid[i]);
        }

        // Bottom row: -I / White / +Q / Black / sub-black / Black / super-white
        // Subdivide 7 bars into same columns
        static const DWORD ntscBot[7] =
        {
            D3DCOLOR_XRGB(0,  33,  76), // -I
            D3DCOLOR_XRGB(255, 255, 255), // 100% White
            D3DCOLOR_XRGB(50,   0,  71), // +Q
            D3DCOLOR_XRGB(7,   7,   7), // PLUGE sub-black
            D3DCOLOR_XRGB(0,   0,   0), // Black
            D3DCOLOR_XRGB(18,  18,  18), // PLUGE super-black
            D3DCOLOR_XRGB(0,   0,   0), // Black
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
        // ----------------------------------------------------------------
        // PAL EBU 8-bar pattern
        // ----------------------------------------------------------------
        // Top 3/4 - 8 full-saturation bars
        // Bottom 1/4 - grey/black reference pairs per column

        const float topH = SH * 0.75f;
        const float barW = SW / 8.f;

        // EBU 100% full-field bars
        static const DWORD palTop[8] =
        {
            D3DCOLOR_XRGB(255, 255, 255), // White
            D3DCOLOR_XRGB(255, 255,   0), // Yellow
            D3DCOLOR_XRGB(0, 255, 255), // Cyan
            D3DCOLOR_XRGB(0, 255,   0), // Green
            D3DCOLOR_XRGB(255,   0, 255), // Magenta
            D3DCOLOR_XRGB(255,   0,   0), // Red
            D3DCOLOR_XRGB(0,   0, 255), // Blue
            D3DCOLOR_XRGB(0,   0,   0), // Black
        };

        for (int i = 0; i < 8; ++i)
        {
            float x0 = barW * (float)i;
            float x1 = barW * (float)(i + 1);
            FillRect(x0, 0.f, x1, topH, palTop[i]);
        }

        // Bottom reference row: alternating grey (IRE 40) / black per column
        static const DWORD palBot[8] =
        {
            D3DCOLOR_XRGB(104, 104, 104), // Grey
            D3DCOLOR_XRGB(0,   0,   0), // Black
            D3DCOLOR_XRGB(104, 104, 104), // Grey
            D3DCOLOR_XRGB(0,   0,   0), // Black
            D3DCOLOR_XRGB(104, 104, 104), // Grey
            D3DCOLOR_XRGB(0,   0,   0), // Black
            D3DCOLOR_XRGB(104, 104, 104), // Grey
            D3DCOLOR_XRGB(0,   0,   0), // Black
        };

        for (int i = 0; i < 8; ++i)
        {
            float x0 = barW * (float)i;
            float x1 = barW * (float)(i + 1);
            FillRect(x0, topH, x1, SH, palBot[i]);
        }
    }

    // Small exit label bottom-left so user knows how to escape
    DrawText(6.f, SH - 14.f, "[B] Back", 1.1f, D3DCOLOR_ARGB(160, 255, 255, 255));

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}



void VideoInfo_OnEnter()
{
    s_prevBtns = 0;
    s_loaded = false;
    s_subState = VSS_INFO;
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
    DrawPageChrome(logo, "VIDEO INFO", "[X] NTSC Bars  [Y] PAL Bars  [B] Back");

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

    DrawRow(COL_L, COL_VL, y1, "MEM CLK :", d.memClkStr);    y1 += LH;
    DrawRow(COL_L, COL_VL, y1, "PIX CLK :", d.pixClkStr);    y1 += LH;
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
        s_loaded = true;
        return;
    }

    WORD cur = GetButtons();

    if (s_subState == VSS_NTSC_BARS || s_subState == VSS_PAL_BARS)
    {
        // Any [B] or [Back] exits the color bar test back to info page
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

    // VSS_INFO
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

    s_prevBtns = cur;
    Render(logo);
}