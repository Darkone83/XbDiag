// DiagCommon.cpp
// Shared rendering + utility implementation for XbDiag.
// See DiagCommon.h for the full API description.

#include "DiagCommon.h"
// Include font.h with macro suppressed so we get the real declaration.
#undef DrawText
#include "font.h"
// Store a direct pointer to the font DrawText BEFORE restoring the macro.
// ScaledDrawText calls s_fontDrawText to avoid infinite recursion.
typedef void (*FontDrawTextFn)(float, float, const char*, float, DWORD);
static FontDrawTextFn s_fontDrawText = DrawText;  // raw font fn, pre-macro
#define DrawText ScaledDrawText
#include <xtl.h>
#include <stdlib.h>     // malloc / free

extern LPDIRECT3DDEVICE8 g_pDevice;

// Video mode info
bool g_isHD = false;
char g_videoModeStr[16] = "480i";

// Resolution scale factors (1.0 = 640x480 design space)
float g_sx = 1.0f;   // SW / 640
float g_sy = 1.0f;   // SH / 480

// ============================================================================
// Ftoi  (no __ftol2_sse)
// ============================================================================

__declspec(noinline) int Ftoi(float f)
{
    int i;
    __asm
    {
        fld   f
        fistp i
    }
    return i;
}

// ============================================================================
// D3D state reset
// ============================================================================

void DiagResetShader()
{
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

// ============================================================================
// 2D filled rectangle primitives
// All use D3DFVF_XYZRHW | D3DFVF_DIFFUSE — no texture.
// ============================================================================

// All primitives accept 640x480 design-space coordinates.
// SX/SY scale to actual backbuffer pixels at runtime.

void FillRect(float x0, float y0, float x1, float y1, DWORD col)
{
    DV v[4] =
    {
        { SX(x0), SY(y0), 0.f, 1.f, col },
        { SX(x1), SY(y0), 0.f, 1.f, col },
        { SX(x0), SY(y1), 0.f, 1.f, col },
        { SX(x1), SY(y1), 0.f, 1.f, col },
    };
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(DV));
}

void FillRectGrad(float x0, float y0, float x1, float y1, DWORD cTop, DWORD cBot)
{
    DV v[4] =
    {
        { SX(x0), SY(y0), 0.f, 1.f, cTop },
        { SX(x1), SY(y0), 0.f, 1.f, cTop },
        { SX(x0), SY(y1), 0.f, 1.f, cBot },
        { SX(x1), SY(y1), 0.f, 1.f, cBot },
    };
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(DV));
}

void HLine(float y, float x0, float x1, DWORD col)
{
    FillRect(x0, y, x1, y + 1.f / g_sy, col);  // 1 physical pixel tall
}

void VLine(float x, float y0, float y1, DWORD col)
{
    FillRect(x, y0, x + 1.f / g_sx, y1, col);  // 1 physical pixel wide
}

// ============================================================================
// String helpers
// ============================================================================

int StrLen(const char* s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

void StrCopy(char* dst, int dstLen, const char* src)
{
    int i = 0;
    while (src[i] && i < dstLen - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

void StrCat2(char* buf, int bufLen, const char* a, const char* b)
{
    int i = 0;
    while (*a && i < bufLen - 1) buf[i++] = *a++;
    while (*b && i < bufLen - 1) buf[i++] = *b++;
    buf[i] = '\0';
}

void StrCat3(char* buf, int bufLen, const char* a, const char* b, const char* c)
{
    int i = 0;
    while (*a && i < bufLen - 1) buf[i++] = *a++;
    while (*b && i < bufLen - 1) buf[i++] = *b++;
    while (*c && i < bufLen - 1) buf[i++] = *c++;
    buf[i] = '\0';
}

void IntToStr(int v, char* buf, int bufLen)
{
    if (bufLen <= 1) return;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }

    // Handle negative
    int out = 0;
    if (v < 0 && out < bufLen - 1) { buf[out++] = '-'; v = -v; }

    char tmp[16];
    int  n = 0;
    unsigned u = (unsigned)v;
    while (u > 0 && n < 15) { tmp[n++] = '0' + (u % 10); u /= 10; }

    for (int i = n - 1; i >= 0 && out < bufLen - 1; --i)
        buf[out++] = tmp[i];
    buf[out] = '\0';
}

void UIntToStr(unsigned v, char* buf, int bufLen)
{
    if (bufLen <= 1) return;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }

    char tmp[16];
    int  n = 0;
    while (v > 0 && n < 15) { tmp[n++] = '0' + (v % 10); v /= 10; }

    int out = 0;
    for (int i = n - 1; i >= 0 && out < bufLen - 1; --i)
        buf[out++] = tmp[i];
    buf[out] = '\0';
}

static const char s_hexChars[] = "0123456789ABCDEF";

void IntToHex(unsigned v, int digits, char* buf, int bufLen)
{
    // Write zero-padded hex, up to 8 digits, most-significant first
    if (digits > 8)  digits = 8;
    if (digits < 1)  digits = 1;
    if (bufLen < digits + 1) return;

    for (int i = digits - 1; i >= 0; --i)
    {
        buf[i] = s_hexChars[v & 0xF];
        v >>= 4;
    }
    buf[digits] = '\0';
}

void HexByte(unsigned char b, char* out2)
{
    out2[0] = s_hexChars[(b >> 4) & 0xF];
    out2[1] = s_hexChars[b & 0xF];
    out2[2] = '\0';
}

// ============================================================================
// Text measurement + drawing helpers
// ============================================================================

// TW measures in design-space pixels (pre-scale) so layout math stays in
// 640x480 units. ScaledDrawText applies SX/SY/SS before calling font's DrawText.

float TW(const char* s, float scale)
{
    if (!s) return 0.f;
    // 6 design-pixels per char * scale gives width in design space
    return (float)StrLen(s) * 6.0f * scale;
}

void ScaledDrawText(float x, float y, const char* text, float scale, DWORD color)
{
    // Call font's DrawText via the captured pointer - NOT via the DrawText macro
    // which would recurse back into ScaledDrawText infinitely.
    float ss = SS(scale);
    s_fontDrawText(SX(x), SY(y), text, ss, color);
}

void DrawTextR(float rightX, float y, const char* s, float sc, DWORD col)
{
    // TW is in design space, SX converts right edge to screen space
    ScaledDrawText(rightX - TW(s, sc), y, s, sc, col);
}

void DrawLabelValue(float x, float y,
    const char* label, const char* value,
    float sc, DWORD labelCol, DWORD valueCol)
{
    ScaledDrawText(x, y, label, sc, labelCol);
    ScaledDrawText(x + TW(label, sc), y, value, sc, valueCol);
}

// ============================================================================
// DDS loader  (uncompressed A8R8G8B8 only)
// Lifted verbatim from BiosScreen with minimal rename.
// ============================================================================

#pragma pack(push,1)
struct DDSPF
{
    DWORD size, flags, fourCC, rgbBitCount;
    DWORD rMask, gMask, bMask, aMask;
};
struct DDSHDR
{
    DWORD size, flags, height, width;
    DWORD pitchOrLinearSize, depth, mipMapCount;
    DWORD reserved1[11];
    DDSPF ddspf;
    DWORD caps, caps2, caps3, caps4, reserved2;
};
#pragma pack(pop)

// ============================================================================
// SwizzleRect
// Replaces XGSwizzleRect to avoid xgraphics.lib dependency.
// Converts a linear ARGB bitmap into Xbox NV2A swizzled layout.
// Only handles 32bpp (4 bytes/pixel), power-of-two dimensions.
// ============================================================================

static unsigned int SwizzleAxis(unsigned int val, int dimension)
{
    unsigned int result = 0;
    for (int bit = 0; dimension > 1; dimension >>= 1, bit++)
    {
        result |= (val & 1) << bit;
        val >>= 1;
        bit++;   // interleave: skip to next output bit pair
    }
    return result;
}

static void SwizzleRect(const BYTE* src, int srcPitch,
    BYTE* dst, int w, int h)
{
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            // Interleave x and y bits to form the swizzled offset
            unsigned int sx = 0, sy = 0;
            unsigned int tx = (unsigned int)x;
            unsigned int ty = (unsigned int)y;

            for (int bit = 0; (1 << bit) < (w > h ? w : h); ++bit)
            {
                if ((1 << bit) < w) { sx |= (tx & 1) << bit; tx >>= 1; }
                if ((1 << bit) < h) { sy |= (ty & 1) << bit; ty >>= 1; }
            }

            unsigned int dstOffset = 0;
            unsigned int srcBitX = 0;
            unsigned int srcBitY = 0;
            int          wBits = 0;
            int          hBits = 0;
            int          tw = w, th = h;
            while (tw > 1) { wBits++; tw >>= 1; }
            while (th > 1) { hBits++; th >>= 1; }

            // Interleave x bits into even positions, y bits into odd
            for (int bit = 0; bit < (wBits > hBits ? wBits : hBits); ++bit)
            {
                if (bit < wBits) dstOffset |= ((x >> bit) & 1) << (bit * 2);
                if (bit < hBits) dstOffset |= ((y >> bit) & 1) << (bit * 2 + 1);
            }

            const BYTE* s = src + y * srcPitch + x * 4;
            BYTE* d = dst + dstOffset * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
}

LPDIRECT3DTEXTURE8 DiagLoadDDS(const char* path, int& outW, int& outH)
{
    outW = outH = 0;
    if (!g_pDevice || !path) return NULL;

    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return NULL;

    DWORD br = 0;
    DWORD magic = 0;
    ReadFile(hf, &magic, 4, &br, NULL);
    if (br != 4 || magic != 0x20534444) { CloseHandle(hf); return NULL; }

    DDSHDR hdr;
    ReadFile(hf, &hdr, sizeof(hdr), &br, NULL);
    if (br != sizeof(hdr) || hdr.size != 124) { CloseHandle(hf); return NULL; }

    // Accept only uncompressed 32-bit ARGB
    const DWORD DDPF_RGB = 0x40;
    if (hdr.ddspf.rgbBitCount != 32 || !(hdr.ddspf.flags & DDPF_RGB))
    {
        CloseHandle(hf); return NULL;
    }

    int w = (int)hdr.width;
    int h = (int)hdr.height;
    if (w <= 0 || h <= 0 || (w & (w - 1)) != 0) { CloseHandle(hf); return NULL; }

    DWORD bytes = (DWORD)(w * h * 4);
    BYTE* px = (BYTE*)malloc(bytes);
    if (!px) { CloseHandle(hf); return NULL; }

    ReadFile(hf, px, bytes, &br, NULL);
    CloseHandle(hf);
    if (br != bytes) { free(px); return NULL; }

    LPDIRECT3DTEXTURE8 tex = NULL;
    if (FAILED(g_pDevice->CreateTexture((UINT)w, (UINT)h, 1, 0,
        D3DFMT_A8R8G8B8, 0, &tex)))
    {
        free(px); return NULL;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, NULL, 0)))
    {
        tex->Release(); free(px); return NULL;
    }

    SwizzleRect(px, w * 4, (BYTE*)lr.pBits, w, h);
    tex->UnlockRect(0);
    free(px);

    outW = w; outH = h;
    return tex;
}

// ============================================================================
// Logo draw  (textured quad, alpha blend)
// ============================================================================

void DrawLogo(const DiagLogo& logo,
    float cx, float cy, float dispW, float dispH, BYTE alpha)
{
    if (!logo.tex) return;

    float l = SX(cx - dispW * 0.5f);
    float r = SX(cx + dispW * 0.5f);
    float t = SY(cy - dispH * 0.5f);
    float b = SY(cy + dispH * 0.5f);
    DWORD col = D3DCOLOR_ARGB(alpha, 255, 255, 255);

    DVT v[4] =
    {
        { l, t, 0.f, 1.f, col, 0.f, 0.f },
        { r, t, 0.f, 1.f, col, 1.f, 0.f },
        { l, b, 0.f, 1.f, col, 0.f, 1.f },
        { r, b, 0.f, 1.f, col, 1.f, 1.f },
    };

    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    g_pDevice->SetTexture(0, logo.tex);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(DVT));

    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

// ============================================================================
// DrawPageChrome
// Draws the full frame chrome shared by every screen:
//   - Background clear
//   - Top gradient bar  (logo left, title right-aligned)
//   - Accent line below top bar
//   - Bottom gradient bar  (hints text)
//   - Border separator lines
// After this call the vertex shader is D3DFVF_XYZRHW|D3DFVF_DIFFUSE (no tex).
// ============================================================================

void DrawPageChrome(const DiagLogo& logo,
    const char* title,
    const char* hints)
{
    // --- Background ---
    g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET,
        COL_BG, 1.f, 0);

    // --- Top bar ---
    FillRectGrad(0.f, 0.f, SW, TOP_BAR_H,
        D3DCOLOR_XRGB(30, 55, 110),
        D3DCOLOR_XRGB(18, 35, 75));
    HLine(TOP_BAR_H, 0.f, SW, COL_BORDER);

    // Accent line just below top bar
    HLine(TOP_BAR_H + 1.f, 0.f, SW, D3DCOLOR_XRGB(60, 100, 200));

    // --- Bottom bar ---
    HLine(BOT_BAR_Y - 1.f, 0.f, SW, COL_BORDER);
    FillRectGrad(0.f, BOT_BAR_Y, SW, SH,
        D3DCOLOR_XRGB(18, 18, 35),
        D3DCOLOR_XRGB(10, 10, 22));

    // --- Logo (top-left) ---
    // Source texture is 256x256 (square). Display size must also be square
    // or the logo gets squashed. Height fits inside the 58px top bar with
    // comfortable padding; width matches to preserve 1:1 aspect ratio.
    const float LOGO_DISP_W = 48.f;
    const float LOGO_DISP_H = 48.f;
    const float LOGO_CX = 20.f + LOGO_DISP_W * 0.5f;
    const float LOGO_CY = TOP_BAR_H * 0.5f;

    DrawLogo(logo, LOGO_CX, LOGO_CY, LOGO_DISP_W, LOGO_DISP_H, 255);

    // Restore shader before any text / rect draws
    DiagResetShader();

    // Divider after logo
    float divX = LOGO_CX + LOGO_DISP_W * 0.5f + 8.f;
    VLine(divX, 4.f, TOP_BAR_H - 4.f, COL_BORDER);

    // --- Title in top bar (right-aligned) ---
    const float TS = 1.3f;
    float barTextY = (TOP_BAR_H - 7.f * TS) * 0.5f;
    DrawTextR(SW - 10.f, barTextY, title, TS, COL_WHITE);

    // --- Hints in bottom bar (left) ---
    float botY = BOT_BAR_Y + (BOT_BAR_H - 7.f * 1.3f) * 0.5f;
    DrawText(LM, botY, hints, 1.3f, COL_YELLOW);

    // --- SD / HD badge (bottom bar, right) ---
    // Badge color: cyan for HD modes, gray for SD
    DWORD badgeCol = g_isHD ? COL_CYAN : COL_GRAY;
    DrawTextR(SW - LM, botY, g_videoModeStr, 1.3f, badgeCol);
}

// ============================================================================
// SMBus helpers
//
// Use HalReadSMBusValue / HalWriteSMBusValue (kernel exports, EXPORTNUM 45/46).
// These are arbitrated by the kernel — safe to call alongside dashboard/PIC
// activity without bus collisions.  The kernel retries on arbitration loss.
//
// ADDRESS CONVENTION: HalReadSMBusValue uses SOFTWARE-SHIFTED 8-bit addresses
// (the 7-bit hardware address left-shifted by 1), matching the I2C wire format
// for write operations.  This is confirmed by PrometheOS (Team Resurgent) which
// is the authoritative reference for Xbox kernel HAL usage.
//
//   Device            7-bit hw    8-bit sw (use this)
//   ─────────────────────────────────────────────────
//   PIC16L / SMC      0x10        0x20   SMBDEV_PIC16L
//   Video enc Conexant 0x45       0x8A   SMBDEV_VIDEO_ENC_CNXT
//   Video enc Focus    0x6A       0xD4   SMBDEV_VIDEO_ENC_FOCUS
//   Video enc Xcalibur 0x70       0xE0   SMBDEV_VIDEO_ENC_XCAL
//   ADM1032 temp      0x4C        0x98   SMBDEV_TEMP_MON  (absent on 1.6)
//   EEPROM            0x54        0xA8   SMBDEV_EEPROM
//   ICS clock         0x69        0xD2
//
// HalReadSMBusValue  (EXPORTNUM 45)
//   ULONG HalReadSMBusValue(UCHAR Address, UCHAR Command,
//                           BOOLEAN WordFlag, PULONG Value);
//   Returns 0 (STATUS_SUCCESS) on success.
//
// HalWriteSMBusValue (EXPORTNUM 46)
//   ULONG HalWriteSMBusValue(UCHAR Address, UCHAR Command,
//                            BOOLEAN WordFlag, ULONG Value);
//   Returns 0 (STATUS_SUCCESS) on success.
//
// Convenience address defines (8-bit software-shifted):
#define SMBADDR_PIC       0x20   // PIC16L / SMC
#define SMBADDR_ENC_CNXT  0x8A   // Conexant video encoder
#define SMBADDR_ENC_FOCUS 0xD4   // Focus FS454 video encoder
#define SMBADDR_ENC_XCAL  0xE0   // Xcalibur video encoder (1.6)
#define SMBADDR_ADM1032   0x98   // ADM1032 temp monitor (1.0-1.5 only)
#define SMBADDR_EEPROM    0xA8   // 93LC56 EEPROM
#define SMBADDR_ICS       0xD2   // ICS clock generator
// ============================================================================

// Forward declarations — provided by xboxkrnl via xtl.h
extern "C"
{
    ULONG __stdcall HalReadSMBusValue(UCHAR Address, UCHAR Command,
        BOOLEAN WordFlag, PULONG Value);
    ULONG __stdcall HalWriteSMBusValue(UCHAR Address, UCHAR Command,
        BOOLEAN WordFlag, ULONG Value);
}

bool SMBusRead(BYTE addr, BYTE reg, BYTE& outVal)
{
    outVal = 0;
    ULONG v = 0;
    if (HalReadSMBusValue(addr, reg, FALSE, &v) != 0)
        return false;
    outVal = (BYTE)(v & 0xFF);
    return true;
}

bool SMBusReadWord(BYTE addr, BYTE reg, WORD& outVal)
{
    outVal = 0;
    ULONG v = 0;
    if (HalReadSMBusValue(addr, reg, TRUE, &v) != 0)
        return false;
    outVal = (WORD)(v & 0xFFFF);
    return true;
}

bool SMBusWrite(BYTE addr, BYTE reg, BYTE val)
{
    return HalWriteSMBusValue(addr, reg, FALSE, (ULONG)val) == 0;
}