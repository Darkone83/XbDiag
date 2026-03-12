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
    char refreshStr[12]; // "60 Hz" / "50 Hz"

    // Encoder (SMBus 0x10 reg 0x00)
    bool encAck;
    BYTE encIdByte;
    char encName[28];    // "Conexant CX25871" etc
    char encIdStr[8];    // "0x76" etc

    // AV pack (SMBus 0x45 reg 0x04)
    char avPack[24];     // "Composite" / "HDTV" / "S-Video" etc
};

static VideoData s_data;
static WORD      s_prevBtns;
static bool      s_loaded = false;

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
    int   whole = Ftoi(v);
    int   frac = Ftoi((v - (float)whole) * 100.f + 0.5f);
    char  t[12];
    IntToStr(whole, t, sizeof(t));
    StrCopy(out, outLen, t);
    StrCat2(out, outLen, out, ".");
    // Zero-pad frac to 2 digits
    if (frac < 10) StrCat2(out, outLen, out, "0");
    IntToStr(frac, t, sizeof(t));
    StrCat2(out, outLen, out, t);
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
                case D3DFMT_X8R8G8B8: StrCopy(d.bppStr, sizeof(d.bppStr), "32bpp"); break;
                case D3DFMT_R5G6B5:
                case D3DFMT_X1R5G5B5:
                case D3DFMT_A1R5G5B5: StrCopy(d.bppStr, sizeof(d.bppStr), "16bpp"); break;
                default:              StrCopy(d.bppStr, sizeof(d.bppStr), "?bpp");  break;
                }
            }
            pBB->Release();
        }

        // Infer refresh rate from video mode string
        // "576i PAL" = 50 Hz, everything else = 60 Hz
        {
            bool isPAL = (g_videoModeStr[0] == '5' &&
                g_videoModeStr[1] == '7' &&
                g_videoModeStr[2] == '6');
            char t[8];
            IntToStr(isPAL ? 50 : 60, t, sizeof(t));
            StrCopy(d.refreshStr, sizeof(d.refreshStr), t);
            StrCat2(d.refreshStr, sizeof(d.refreshStr), d.refreshStr, " Hz");
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
}

// ============================================================================
// OnEnter
// ============================================================================

void VideoInfo_OnEnter()
{
    s_prevBtns = 0;
    s_loaded = false;
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
    DrawPageChrome(logo, "VIDEO INFO", "[B] Back");

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

    // ---- RIGHT: ENCODER ----
    DrawText(COL_R, y2, "ENCODER", 1.3f, COL_YELLOW);
    HLine(y2 + LH + 1.f, COL_R, SW - LM, COL_BORDER);
    y2 += LH + 6.f;

    DrawRow(COL_R, COL_VR, y2, "CHIP    :", d.encName,
        d.encAck ? COL_CYAN : COL_YELLOW);                 y2 += LH;
    DrawRow(COL_R, COL_VR, y2, "CHIP ID :", d.encIdStr,
        d.encAck ? COL_WHITE : COL_GRAY);                  y2 += LH;

    // Encoder capabilities note
    if (d.encAck)
    {
        const char* caps = NULL;
        switch (d.encIdByte)
        {
        case 0x76: caps = "Composite / S-Video / SCART"; break;
        case 0x54: caps = "Component / 480p / 1080i";    break;
        case 0x09: caps = "Component / 480p / 720p";     break;
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

    if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
    {
        RequestState(MSTATE_MENU);
        s_prevBtns = cur;
        return;
    }

    s_prevBtns = cur;
    Render(logo);
}