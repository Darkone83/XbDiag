// HttpCap.cpp -- XbDiag framebuffer capture for HTTP live view
//
// Approach: CopyRects detiles GPU surface into linear system memory,
// then we send the raw pixel data at native resolution as a BMP.
// The browser handles all scaling via CSS. No Xbox-side scaling.

#include "HttpCap.h"
#include "DiagCommon.h"

extern void PageStart(const char* title, const char* tab, const char* extraHead);
extern void PageEnd();
extern void BA(const char* s);
extern void SendHTML(SOCKET c);

typedef enum { CAP_IDLE = 0, CAP_REQUESTED = 1, CAP_CAPTURING = 2, CAP_READY = 3 } CapState;
static volatile int       s_state = CAP_IDLE;
static DWORD              s_lastCapMs = 0;
static IDirect3DSurface8* s_sysMem = NULL;

// Pixel buffer for scaled output -- large enough for supported Xbox output modes
static BYTE s_pixBuf[1280 * 720 * 4];
static int  s_capW = 0;
static int  s_capH = 0;

static int  CLen(const char* s) { int n = 0; while (s[n])++n; return n; }
static bool CEq(const char* a, const char* b)
{
    int i = 0; while (a[i] && b[i] && a[i] == b[i])++i; return a[i] == b[i];
}
static void CCopy(char* d, int dl, const char* s)
{
    int i = 0; while (i < dl - 1 && s[i]) { d[i] = s[i]; ++i; } d[i] = '\0';
}
static void CNum(int v, char* buf, int bl)
{
    char rev[16]; int ri = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (v) { rev[ri++] = '0' + (char)(v % 10); v /= 10; }
    int i = 0; while (ri > 0 && i < bl - 1)buf[i++] = rev[--ri]; buf[i] = '\0';
}
static void WLE32(BYTE* p, DWORD v)
{
    p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24);
}
static void WLE16(BYTE* p, WORD v)
{
    p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8);
}

static void BuildBmpHdr(BYTE* h, int w, int hh)
{
    int rowPad = (w * 3 + 3) & ~3;
    DWORD imgB = (DWORD)(rowPad * hh);
    DWORD fileB = 54 + imgB;
    h[0] = 'B'; h[1] = 'M';
    WLE32(h + 2, fileB); WLE32(h + 6, 0); WLE32(h + 10, 54);
    WLE32(h + 14, 40);
    WLE32(h + 18, (DWORD)w); WLE32(h + 22, (DWORD)hh);
    WLE16(h + 26, 1); WLE16(h + 28, 24);
    WLE32(h + 30, 0); WLE32(h + 34, imgB);
    WLE32(h + 38, 2835); WLE32(h + 42, 2835);
    WLE32(h + 46, 0); WLE32(h + 50, 0);
}

void HttpCap_Init() { s_state = CAP_IDLE; s_lastCapMs = 0; s_sysMem = NULL; s_capW = 0; s_capH = 0; }
bool HttpCap_IsReady() { return s_state == CAP_READY; }

void HttpCap_RequestFrame()
{
    DWORD now = GetTickCount();
    if (s_state == CAP_IDLE && (now - s_lastCapMs) >= HTTPCAP_MIN_INTERVAL_MS)
        s_state = CAP_REQUESTED;
}

void HttpCap_CaptureFrame()
{
    if (s_state != CAP_REQUESTED) return;
    s_state = CAP_CAPTURING;

    IDirect3DSurface8* pBack = NULL;
    if (FAILED(g_pDevice->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &pBack)) || !pBack)
    {
        s_state = CAP_IDLE; return;
    }

    D3DSURFACE_DESC desc;
    pBack->GetDesc(&desc);

    // Pre-allocate system memory surface once, reuse every capture
    if (!s_sysMem)
    {
        if (FAILED(g_pDevice->CreateImageSurface(
            desc.Width, desc.Height, desc.Format, &s_sysMem)) || !s_sysMem)
        {
            pBack->Release(); s_state = CAP_IDLE; return;
        }
    }

    // CopyRects detiles GPU tiled surface into linear system memory
    if (FAILED(g_pDevice->CopyRects(pBack, NULL, 0, s_sysMem, NULL)))
    {
        pBack->Release(); s_state = CAP_IDLE; return;
    }
    pBack->Release();

    D3DLOCKED_RECT lr;
    if (FAILED(s_sysMem->LockRect(&lr, NULL, 0)))
    {
        s_state = CAP_IDLE; return;
    }

    int srcW = (int)desc.Width;
    int srcH = (int)desc.Height;
    BYTE* src = (BYTE*)lr.pBits;
    int pitch = lr.Pitch;

    // Balanced browser-live capture sizing.
    // 640x480  -> 480x360
    // 1280x720 -> 640x360
    // Smaller modes remain native.
    int w = srcW;
    if (srcW >= 1000)
        w = 640;
    else if (srcW > 480)
        w = 480;

    int h = (srcH * w) / srcW;
    if (h < 1) h = 1;

    // Copy BGRA bottom-up for BMP with nearest-neighbor proportional scaling.
    // This keeps text readable while reducing pixel work and TCP transfer size.
    for (int y = 0; y < h; ++y)
    {
        int sy = (y * srcH) / h;
        int dstRow = h - 1 - y;
        BYTE* dstLine = s_pixBuf + dstRow * w * 4;
        BYTE* srcLine = src + sy * pitch;

        for (int x = 0; x < w; ++x)
        {
            int sx = (x * srcW) / w;
            BYTE* p = srcLine + sx * 4;

            dstLine[x * 4 + 0] = p[0];
            dstLine[x * 4 + 1] = p[1];
            dstLine[x * 4 + 2] = p[2];
            dstLine[x * 4 + 3] = 0xFF;
        }
    }

    s_sysMem->UnlockRect();
    s_capW = w; s_capH = h;
    s_lastCapMs = GetTickCount();
    s_state = CAP_READY;
}

void HttpCap_ServeScreenshot(SOCKET c)
{
    // Request a capture for next time
    HttpCap_RequestFrame();

    // Serve whatever is in the buffer -- no blocking wait.
    // If nothing ready yet, return 503 and browser retries.
    if (s_state != CAP_READY || s_capW == 0)
    {
        const char* e = "HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(c, e, CLen(e), 0);
        return;
    }

    BYTE bmpHdr[54];
    BuildBmpHdr(bmpHdr, s_capW, s_capH);

    int rowPad = (s_capW * 3 + 3) & ~3;
    DWORD imgB = (DWORD)(rowPad * s_capH);
    DWORD fileB = 54 + imgB;

    char szFile[12]; CNum((int)fileB, szFile, sizeof(szFile));
    char hdr[192];
    CCopy(hdr, sizeof(hdr),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: image/bmp\r\n"
        "Cache-Control: no-cache, no-store\r\n"
        "Content-Length: ");
    int hl = CLen(hdr);
    CCopy(hdr + hl, sizeof(hdr) - hl, szFile); hl = CLen(hdr);
    CCopy(hdr + hl, sizeof(hdr) - hl, "\r\nConnection: close\r\n\r\n");

    send(c, hdr, CLen(hdr), 0);

    // Build full BMP (header + pixels) in one buffer, send in chunks
    DWORD bufSz = 54 + imgB;
    BYTE* bmpAll = (BYTE*)GlobalAlloc(GMEM_FIXED, bufSz);
    if (!bmpAll) { s_state = CAP_IDLE; return; }

    for (int i = 0; i < 54; ++i) bmpAll[i] = bmpHdr[i];

    for (int row = 0; row < s_capH; ++row)
    {
        BYTE* s2 = s_pixBuf + row * s_capW * 4;
        BYTE* dst = bmpAll + 54 + (DWORD)row * (DWORD)rowPad;
        for (int x = 0; x < s_capW; ++x)
        {
            dst[x * 3 + 0] = s2[x * 4 + 0]; dst[x * 3 + 1] = s2[x * 4 + 1]; dst[x * 3 + 2] = s2[x * 4 + 2];
        }
        for (int p2 = s_capW * 3; p2 < rowPad; ++p2) dst[p2] = 0;
    }

    // Send in chunks of 32KB to avoid overwhelming socket buffer
    const int CHUNK = 32768;
    int sent = 0;
    while (sent < (int)bufSz)
    {
        int toSend = ((int)bufSz - sent) < CHUNK ? ((int)bufSz - sent) : CHUNK;
        int r = send(c, (char*)bmpAll + sent, toSend, 0);
        if (r <= 0) break;
        sent += r;
    }
    GlobalFree(bmpAll);
    s_state = CAP_IDLE;
}

void HttpCap_ServeLivePage(SOCKET c, const char* live)
{
    bool autoRefresh = (live[0] == '1');
    HttpCap_RequestFrame();

    char extraHead[96]; extraHead[0] = '\0';
    if (autoRefresh)
        CCopy(extraHead, sizeof(extraHead),
            "<meta http-equiv=refresh content='5; url=/live?live=1'>");

    PageStart("Live View", "lv", extraHead[0] ? extraHead : NULL);

    // Image at native/scaled capture resolution, browser scales to fit via CSS
    BA("<div style='display:flex;flex-direction:column;align-items:center;gap:16px;width:100%'>"
        "<img src=/screenshot alt='XbDiag Framebuffer' "
        "style='border:1px solid rgba(80,220,255,.15);display:block;"
        "width:100%;height:80vh;object-fit:contain;background:#000'"
        " onerror=\"setTimeout(function(){this.src='/screenshot?t='+Date.now()},2000)\">"
        "<div style='display:flex;gap:10px;align-items:center'>");

    if (autoRefresh)
        BA("<a href='/live?live=0' class='tab on'>Live ON</a>");
    else
        BA("<a href='/live?live=1' class='tab'>Live OFF</a>");

    BA("<span style='width:1px;height:20px;background:rgba(80,220,255,.1)'></span>"
        "<a href='/live?live="); BA(autoRefresh ? "1" : "0");
    BA("' style='padding:6px 18px;background:rgba(80,220,255,.06);"
        "border:1px solid rgba(80,220,255,.25);color:#50DCFF;"
        "text-decoration:none;font-size:11px;letter-spacing:.5px;"
        "text-transform:uppercase'>Capture Now</a>"
        "</div>"
        "<div style='font-size:10px;color:#1A2840;letter-spacing:.5px'>");
    BA(autoRefresh ? "Auto-refresh every 5s" : "Static &mdash; click Capture Now to update");
    BA("</div></div>");

    PageEnd();
    SendHTML(c);
}