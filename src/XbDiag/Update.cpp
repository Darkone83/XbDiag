// Update.cpp
// XbDiag - Software update checker and downloader.
//
// Version check reads local D:\XbDiag.ver and compares against server.
// If no local file or server is newer, downloads all files from server.
// Download uses blocking socket with SO_RCVTIMEO/SO_SNDTIMEO timeouts,
// matching the pattern from xbox_update.cpp HttpDownloadFile.
//
// RXDK constraints:
//   - No sprintf / sscanf / strlen -- use DiagCommon helpers
//   - No inline asm
//   - File-scope statics only
//   - Ftoi() for float-to-int
//   - All sockets non-blocking (FIONBIO) for version check state machine

#include "Update.h"
#include "font.h"
#include "input.h"
#include "FileExplorerMU.h"
#include <xtl.h>
#include <winsockx.h>

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

static void Utf8ToAsciiInPlace(char* buf, int* ioLen)
{
    // di <= si always: multibyte sequences (2-3 bytes) map to 1-3 ASCII chars,
    // so writing back into buf ahead of the read pointer is safe in-place.
    int si = 0, di = 0;
    int srcLen = *ioLen;

    while (si < srcLen)
    {
        unsigned char c = (unsigned char)buf[si];

        if (c < 0x80) { buf[di++] = (char)c; ++si; continue; }

        if (si + 2 < srcLen)
        {
            unsigned char c1 = (unsigned char)buf[si + 1];
            unsigned char c2 = (unsigned char)buf[si + 2];

            // EN DASH / EM DASH : E2 80 93 / E2 80 94
            if (c == 0xE2 && c1 == 0x80 && (c2 == 0x93 || c2 == 0x94))
            {
                buf[di++] = '-'; si += 3; continue;
            }

            // ELLIPSIS : E2 80 A6
            if (c == 0xE2 && c1 == 0x80 && c2 == 0xA6)
            {
                buf[di++] = '.'; buf[di++] = '.'; buf[di++] = '.'; si += 3; continue;
            }

            // RIGHT ARROW : E2 86 92
            if (c == 0xE2 && c1 == 0x86 && c2 == 0x92)
            {
                buf[di++] = '-'; buf[di++] = '>'; si += 3; continue;
            }

            // Single curly quotes : E2 80 98 / E2 80 99
            if (c == 0xE2 && c1 == 0x80 && (c2 == 0x98 || c2 == 0x99))
            {
                buf[di++] = '\''; si += 3; continue;
            }

            // Double curly quotes : E2 80 9C / E2 80 9D
            if (c == 0xE2 && c1 == 0x80 && (c2 == 0x9C || c2 == 0x9D))
            {
                buf[di++] = '"'; si += 3; continue;
            }
        }

        if (si + 1 < srcLen)
        {
            unsigned char c1 = (unsigned char)buf[si + 1];

            // MULTIPLICATION SIGN : C3 97
            if (c == 0xC3 && c1 == 0x97)
            {
                buf[di++] = 'x'; si += 2; continue;
            }

            // NO-BREAK SPACE : C2 A0
            if (c == 0xC2 && c1 == 0xA0)
            {
                buf[di++] = ' '; si += 2; continue;
            }
        }

        // Unknown multibyte — replace with space
        buf[di++] = ' '; ++si;
    }

    buf[di] = '\0';
    *ioLen = di;
}

static void FetchChangelog();

static const char* k_host = "darkone83.myddns.me";
static const char* k_verPath = "/xbdiag/XbDiag.ver";
static char        k_xbeDest[128];
static char        s_verDest[128];
static const int   k_httpPort = 8008;

enum UpdateState
{
    UPST_IDLE = 0,
    UPST_NET_INIT,
    UPST_DNS,
    UPST_CONNECT_VER,
    UPST_SEND_VER,
    UPST_RECV_VER,
    UPST_COMPARE,
    UPST_UP_TO_DATE,
    UPST_AVAIL,
    UPST_RECV_XBE,
    UPST_RECV_VER2,
    UPST_WRITE_DONE,
    UPST_ERROR,
};

static UpdateState s_state = UPST_IDLE;
static WORD        s_prevBtns = 0;
static bool        s_netUp = false;
static XNDNS* s_dns = NULL;
static IN_ADDR     s_serverAddr;
static SOCKET      s_sock = INVALID_SOCKET;
static char        s_recvBuf[4096];
static int         s_recvLen = 0;
static char        s_remoteVer[32];
static char        s_localVer[32];
static bool        s_isNewer = false;
static HANDLE      s_hFile = INVALID_HANDLE_VALUE;
static DWORD       s_xbeTotal = 0;
static DWORD       s_xbeReceived = 0;
static char        s_errorMsg[80];
static bool        s_bootCheckDone = false;
static bool        s_bootFoundUpdate = false;
static DWORD       s_netInitStart = 0;
static bool        s_bootCheckStarted = false;

// Changelog
static char        s_changelog[4096];
static int         s_changelogLen = 0;
static bool        s_changelogFetched = false;
static float       s_changelogScroll = 0.f;
static DWORD       s_changelogLastTick = 0;

static bool IsWrapSpace(char c) { return c == ' ' || c == '\t'; }

// ============================================================================
// String helpers
// ============================================================================

static void ParseVerParts(const char* s, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    int field = 0;
    for (int i = 0; s[i] != '\0' && field < 3; ++i)
    {
        char c = s[i];
        if (c >= '0' && c <= '9')  v[field] = v[field] * 10 + (int)(c - '0');
        else if (c == '.')         ++field;
        else                       break;
    }
}

static int VerCmp(const char* a, const char* b)
{
    int pa[3], pb[3];
    ParseVerParts(a, pa);
    ParseVerParts(b, pb);
    for (int i = 0; i < 3; ++i)
    {
        if (pa[i] < pb[i]) return -1;
        if (pa[i] > pb[i]) return  1;
    }
    return 0;
}

static void StripWhitespace(char* buf)
{
    int len = StrLen(buf);
    while (len > 0)
    {
        char c = buf[len - 1];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') buf[--len] = '\0';
        else break;
    }
    int start = 0;
    while (buf[start] == ' ' || buf[start] == '\t') ++start;
    if (start > 0)
    {
        int i = 0;
        while (buf[start + i]) { buf[i] = buf[start + i]; ++i; }
        buf[i] = '\0';
    }
}

static void AppendStr(char* dst, int dstLen, const char* src)
{
    int dlen = StrLen(dst), slen = StrLen(src), space = dstLen - dlen - 1;
    for (int i = 0; i < slen && i < space; ++i) dst[dlen + i] = src[i];
    dst[dlen + (slen < space ? slen : space)] = '\0';
}

static int FindHeaderEnd(const char* buf, int len)
{
    for (int i = 0; i < len - 3; ++i)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return i + 4;
    return -1;
}

static int GetHttpStatus(const char* buf, int len)
{
    if (len < 12) return 0;
    if (buf[0] != 'H' || buf[1] != 'T' || buf[2] != 'T' || buf[3] != 'P') return 0;
    int i = 0;
    while (i < len && buf[i] != ' ') ++i;
    while (i < len && buf[i] == ' ') ++i;
    if (i + 3 > len) return 0;
    int code = 0;
    for (int j = 0; j < 3 && buf[i + j] >= '0' && buf[i + j] <= '9'; ++j)
        code = code * 10 + (int)(buf[i + j] - '0');
    return code;
}

static DWORD ParseContentLength(const char* buf, int bodyStart)
{
    const char* needle = "content-length:";
    for (int i = 0; i < bodyStart - 15; ++i)
    {
        bool m = true;
        for (int k = 0; needle[k]; ++k)
        {
            char hc = buf[i + k];
            if (hc >= 'A' && hc <= 'Z') hc = (char)(hc + 32);
            if (hc != needle[k]) { m = false; break; }
        }
        if (m)
        {
            const char* p = buf + i + 15;
            while (*p == ' ') ++p;
            DWORD val = 0;
            while (*p >= '0' && *p <= '9') val = val * 10 + (DWORD)(*p++ - '0');
            return val;
        }
    }
    return 0;
}

static const char* FindBody(const char* buf, int len, DWORD* outCL)
{
    *outCL = 0;
    int bodyStart = FindHeaderEnd(buf, len);
    if (bodyStart < 0) return NULL;
    *outCL = ParseContentLength(buf, bodyStart);
    return buf + bodyStart;
}

// ============================================================================
// Network helpers
// ============================================================================

static void NetEnsure()
{
    if (s_netUp) return;
    XNetStartupParams xnsp; ZeroMemory(&xnsp, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    XNetStartup(&xnsp);
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    s_netUp = true;
}

static void CloseSocket()
{
    if (s_sock != INVALID_SOCKET) { closesocket(s_sock); s_sock = INVALID_SOCKET; }
}

static void CloseFile()
{
    if (s_hFile != INVALID_HANDLE_VALUE) { CloseHandle(s_hFile); s_hFile = INVALID_HANDLE_VALUE; }
}

static void SetError(const char* msg)
{
    StrCopy(s_errorMsg, sizeof(s_errorMsg), msg);
    s_state = UPST_ERROR;
    CloseSocket(); CloseFile();
    if (s_dns) { XNetDnsRelease(s_dns); s_dns = NULL; }
}


static bool BeginConnect(int port, UpdateState nextState)
{
    CloseSocket();
    s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_sock == INVALID_SOCKET) { SetError("socket() failed"); return false; }
    u_long nb = 1; ioctlsocket(s_sock, FIONBIO, &nb);
    sockaddr_in sa; ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    sa.sin_addr = s_serverAddr;
    int r = connect(s_sock, (sockaddr*)&sa, sizeof(sa));
    if (r == 0 || WSAGetLastError() == WSAEWOULDBLOCK) { s_state = nextState; return true; }
    SetError("connect() failed"); return false;
}

static bool PollConnect()
{
    fd_set wfds, efds;
    FD_ZERO(&wfds); FD_SET(s_sock, &wfds);
    FD_ZERO(&efds); FD_SET(s_sock, &efds);
    TIMEVAL tv = { 0, 0 };
    int r = select(0, NULL, &wfds, &efds, &tv);
    if (r == SOCKET_ERROR) { SetError("select() failed");     return false; }
    if (FD_ISSET(s_sock, &efds)) { SetError("TCP connect refused"); return false; }
    return FD_ISSET(s_sock, &wfds) != 0;
}

static bool SendGet(const char* path)
{
    char req[256];
    StrCopy(req, sizeof(req), "GET ");
    AppendStr(req, sizeof(req), path);
    AppendStr(req, sizeof(req), " HTTP/1.0\r\nHost: ");
    AppendStr(req, sizeof(req), k_host);
    AppendStr(req, sizeof(req), "\r\nConnection: close\r\n\r\n");
    int total = StrLen(req), sent = 0;
    while (sent < total)
    {
        int n = send(s_sock, req + sent, total - sent, 0);
        if (n == SOCKET_ERROR)
        {
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
            SetError("send() failed"); return false;
        }
        sent += n;
    }
    return true;
}

static void ReadLocalVer()
{
    ZeroMemory(s_localVer, sizeof(s_localVer));
    HANDLE hv = CreateFile(s_verDest, GENERIC_READ,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hv != INVALID_HANDLE_VALUE)
    {
        char buf[64]; DWORD rd = 0;
        if (ReadFile(hv, buf, sizeof(buf) - 1, &rd, NULL) && rd > 0)
        {
            buf[rd] = '\0'; StripWhitespace(buf);
            if (buf[0] != '\0') StrCopy(s_localVer, sizeof(s_localVer), buf);
        }
        CloseHandle(hv);
    }
}

// ============================================================================
// Boot-check polling
// ============================================================================

static void BootCheckPoll()
{
    switch (s_state)
    {
    case UPST_NET_INIT:
    {
        XNADDR xna; ZeroMemory(&xna, sizeof(xna));
        DWORD st = XNetGetTitleXnAddr(&xna);
        if (st == XNET_GET_XNADDR_PENDING)
        {
            if (GetTickCount() - s_netInitStart > 3000) SetError("No network link"); return;
        }
        if ((st & XNET_GET_XNADDR_NONE) || xna.ina.s_addr == 0)
        {
            SetError("No network link"); return;
        }
        int dr = XNetDnsLookup(k_host, NULL, &s_dns);
        if (dr != 0 || !s_dns) { SetError("DNS lookup failed"); return; }
        s_state = UPST_DNS; return;
    }
    case UPST_DNS:
    {
        if (!s_dns) { SetError("DNS handle null"); return; }
        if (s_dns->iStatus == WSAEINPROGRESS) return;
        if (s_dns->iStatus != 0)
        {
            XNetDnsRelease(s_dns); s_dns = NULL; SetError("DNS failed"); return;
        }
        s_serverAddr = s_dns->aina[0];
        XNetDnsRelease(s_dns); s_dns = NULL;
        BeginConnect(k_httpPort, UPST_CONNECT_VER); return;
    }
    case UPST_CONNECT_VER:
    {
        if (!PollConnect()) return;
        s_recvLen = 0; ZeroMemory(s_recvBuf, sizeof(s_recvBuf));
        if (!SendGet(k_verPath)) return;
        s_state = UPST_RECV_VER; return;
    }
    case UPST_RECV_VER:
    {
        int space = (int)sizeof(s_recvBuf) - s_recvLen - 1;
        if (space <= 0) { s_state = UPST_COMPARE; return; }
        int n = recv(s_sock, s_recvBuf + s_recvLen, space, 0);
        if (n > 0) { s_recvLen += n; s_recvBuf[s_recvLen] = '\0'; }
        else if (n == 0) { CloseSocket(); s_state = UPST_COMPARE; }
        else { if (WSAGetLastError() != WSAEWOULDBLOCK) SetError("recv error"); }
        return;
    }
    case UPST_COMPARE:
    {
        int status = GetHttpStatus(s_recvBuf, s_recvLen);
        if (status != 200)
        {
            char msg[40]; StrCopy(msg, sizeof(msg), "HTTP error ");
            char c[8]; IntToStr(status, c, sizeof(c)); AppendStr(msg, sizeof(msg), c);
            SetError(msg); return;
        }
        DWORD cl = 0; const char* body = FindBody(s_recvBuf, s_recvLen, &cl);
        if (!body) { SetError("Bad HTTP response"); return; }
        StrCopy(s_remoteVer, sizeof(s_remoteVer), body); StripWhitespace(s_remoteVer);
        if (s_remoteVer[0] == '\0') { SetError("Empty version response"); return; }
        s_isNewer = (s_localVer[0] == '\0' || VerCmp(s_remoteVer, s_localVer) > 0);
        s_bootFoundUpdate = s_isNewer; s_bootCheckDone = true;
        s_state = s_isNewer ? UPST_AVAIL : UPST_UP_TO_DATE; return;
    }
    default: s_bootCheckDone = true; return;
    }
}

// ============================================================================
// Public API
// ============================================================================

void Update_SetPaths(const char* xbeDir)
{
    if (xbeDir && xbeDir[0] != '\0')
    {
        StrCopy(k_xbeDest, sizeof(k_xbeDest), xbeDir);
        AppendStr(k_xbeDest, sizeof(k_xbeDest), "default.xbe");
        StrCopy(s_verDest, sizeof(s_verDest), xbeDir);
        AppendStr(s_verDest, sizeof(s_verDest), "XbDiag.ver");
    }
}

void Update_StartBootCheck()
{
    if (s_bootCheckStarted) return;
    s_bootCheckStarted = true; s_bootCheckDone = false; s_bootFoundUpdate = false;
    ZeroMemory(s_remoteVer, sizeof(s_remoteVer)); ZeroMemory(s_errorMsg, sizeof(s_errorMsg));
    s_recvLen = 0;
    FE_MU_MountAll();
    if (k_xbeDest[0] == '\0') Update_SetPaths("D:\\");
    ReadLocalVer();
    NetEnsure();
    s_netInitStart = GetTickCount();
    s_state = UPST_NET_INIT;
}

bool Update_IsCheckComplete()
{
    if (!s_bootCheckStarted) return true;
    if (!s_bootCheckDone) BootCheckPoll();
    return s_bootCheckDone;
}

bool Update_BootFoundUpdate() { return s_bootFoundUpdate; }
const char* Update_GetLocalVersion() { return s_localVer[0] ? s_localVer : "unknown"; }

void Update_OnEnter()
{
    s_prevBtns = GetButtons();
    ZeroMemory(s_errorMsg, sizeof(s_errorMsg));
    ZeroMemory(s_changelog, sizeof(s_changelog));
    s_changelogLen = 0;
    s_changelogFetched = false;
    s_changelogScroll = 0.f;
    s_changelogLastTick = 0;
    CloseFile();
    if (k_xbeDest[0] == '\0') Update_SetPaths("D:\\");
    if (s_state == UPST_AVAIL || s_state == UPST_UP_TO_DATE || s_state == UPST_WRITE_DONE)
    {
        // DNS already resolved during boot check — fetch changelog now
        FetchChangelog();
        return;
    }
    if (s_state == UPST_ERROR || s_state == UPST_IDLE)
    {
        s_state = UPST_IDLE; s_recvLen = 0; s_xbeTotal = 0; s_xbeReceived = 0;
        ZeroMemory(s_remoteVer, sizeof(s_remoteVer)); return;
    }
}

// ============================================================================
// Render helpers
// ============================================================================

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

static void DrawProgressBar(float x, float y, float w, float h, float frac)
{
    FillRect(x, y, x + w, y + h, D3DCOLOR_XRGB(20, 24, 52));
    HLine(y, x, x + w, COL_BORDER); HLine(y + h, x, x + w, COL_BORDER);
    VLine(x, y, y + h, COL_BORDER); VLine(x + w, y, y + h, COL_BORDER);
    if (frac < 0.f) frac = 0.f;
    if (frac > 1.f) frac = 1.f;
    float fillW = (w - 2.f) * frac;
    if (fillW > 0.f)
        FillRectGrad(x + 1.f, y + 1.f, x + 1.f + fillW, y + h - 1.f,
            D3DCOLOR_XRGB(40, 120, 220), D3DCOLOR_XRGB(20, 80, 160));
    int pct = Ftoi(frac * 100.f);
    char pctBuf[8]; IntToStr(pct, pctBuf, sizeof(pctBuf));
    char pctStr[12]; StrCopy(pctStr, sizeof(pctStr), pctBuf); AppendStr(pctStr, sizeof(pctStr), "%");
    float tw = TW(pctStr, 1.2f);
    DrawText(x + (w - tw) * 0.5f, y + (h - LINE_H) * 0.5f, pctStr, 1.2f, COL_WHITE);
}

static void Render(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    const char* hint = "[B] Back";
    if (s_state == UPST_IDLE)       hint = "[A] Check for update    [B] Back";
    else if (s_state == UPST_AVAIL)      hint = "[A] Download update    [B] Back";
    else if (s_state == UPST_WRITE_DONE) hint = "[A] Relaunch to dashboard";
    else if (s_state == UPST_RECV_XBE || s_state == UPST_RECV_VER2) hint = "";
    else if (s_state == UPST_UP_TO_DATE) hint = "[A] Re-check    [B] Back";
    else if (s_state == UPST_ERROR)      hint = "[A] Retry    [B] Back";

    DrawPageChrome(logo, "SOFTWARE UPDATE", hint);

    const float LX = LM, VX = LM + 130.f, LH = LINE_H + 2.f;
    float y = CONTENT_Y + 16.f;

    DrawText(LX, y, "LOCAL VERSION :", 1.2f, COL_GRAY);
    DrawText(VX, y, s_localVer[0] ? s_localVer : "---", 1.2f, COL_CYAN);
    y += LH;

    DrawText(LX, y, "REMOTE VERSION:", 1.2f, COL_GRAY);
    {
        const char* remStr = s_remoteVer[0] ? s_remoteVer : "---";
        DWORD remCol = COL_DIM;
        if (s_remoteVer[0]) remCol = s_isNewer ? COL_YELLOW : COL_GREEN;
        DrawText(VX, y, remStr, 1.2f, remCol);
    }
    y += LH * 2.f;

    DrawText(LX, y, "STATUS        :", 1.2f, COL_GRAY);
    {
        const char* ss = ""; DWORD sc = COL_WHITE;
        switch (s_state)
        {
        case UPST_IDLE:        ss = "Press [A] to check for updates"; sc = COL_DIM;    break;
        case UPST_NET_INIT:    ss = "Waiting for network link...";    sc = COL_YELLOW; break;
        case UPST_DNS:         ss = "Resolving host...";              sc = COL_YELLOW; break;
        case UPST_CONNECT_VER: ss = "Connecting...";                  sc = COL_YELLOW; break;
        case UPST_SEND_VER:
        case UPST_RECV_VER:    ss = "Fetching version info...";       sc = COL_YELLOW; break;
        case UPST_COMPARE:     ss = "Comparing versions...";          sc = COL_YELLOW; break;
        case UPST_UP_TO_DATE:  ss = "XbDiag is up to date";           sc = COL_GREEN;  break;
        case UPST_AVAIL:       ss = "Update available!  Press [A] to download"; sc = COL_YELLOW; break;
        case UPST_RECV_XBE:    ss = "Downloading update...";          sc = COL_CYAN;   break;
        case UPST_RECV_VER2:   ss = "Downloading files...";           sc = COL_CYAN;   break;
        case UPST_WRITE_DONE:  ss = "Update complete!  Relaunch the app for the new version."; sc = COL_GREEN; break;
        case UPST_ERROR:       ss = s_errorMsg; sc = COL_RED; break;
        default: break;
        }
        DrawText(VX, y, ss, 1.2f, sc);
    }
    y += LH * 2.f;

    if (s_state == UPST_RECV_XBE || s_state == UPST_WRITE_DONE)
    {
        const float BAR_W = SW - LM * 2.f, BAR_H = 22.f;
        DrawText(LX, y, "PROGRESS      :", 1.2f, COL_GRAY); y += LH;
        float frac = 0.f;
        if (s_xbeTotal > 0) frac = (float)s_xbeReceived / (float)s_xbeTotal;
        else if (s_state == UPST_WRITE_DONE) frac = 1.f;
        DrawProgressBar(LX, y, BAR_W, BAR_H, frac);
        y += BAR_H + 8.f;
        char rcvBuf[16]; IntToStr((int)(s_xbeReceived / 1024), rcvBuf, sizeof(rcvBuf));
        char totBuf[16]; IntToStr((int)(s_xbeTotal / 1024), totBuf, sizeof(totBuf));
        char byteStr[40]; StrCopy(byteStr, sizeof(byteStr), rcvBuf); AppendStr(byteStr, sizeof(byteStr), " KB");
        if (s_xbeTotal > 0)
        {
            AppendStr(byteStr, sizeof(byteStr), " / ");
            AppendStr(byteStr, sizeof(byteStr), totBuf);
            AppendStr(byteStr, sizeof(byteStr), " KB");
        }
        DrawText(LX, y, byteStr, 1.1f, COL_DIM);
        y += LH;
    }

    // ---- Changelog frame ----
    {
        const float CX = LM;
        const float CW = SW - LM * 2.f;
        const float CT = y + 4.f;
        const float CH = BOT_BAR_Y - CT - 8.f;
        if (CH > 20.f)
        {
            // Frame border
            HLine(CT, CX, CX + CW, COL_BORDER);
            HLine(CT + CH, CX, CX + CW, COL_BORDER);
            VLine(CX, CT, CT + CH, COL_BORDER);
            VLine(CX + CW, CT, CT + CH, COL_BORDER);

            // Title
            DrawText(CX + 6.f, CT - LINE_H * 0.5f, "CHANGE LOG", 1.0f, COL_GRAY);

            if (s_changelogLen > 0)
            {
                // Auto-scroll — advance every 80ms
                DWORD now = GetTickCount();
                if (now - s_changelogLastTick > 80)
                {
                    s_changelogScroll += 0.5f;
                    s_changelogLastTick = now;
                }

                const float textX = CX + 6.f;
                const float maxW = CW - 12.f;

                // Draw lines clipped to frame
                float lineY = CT + LINE_H - s_changelogScroll;
                const char* p = s_changelog;
                float totalH = 0.f;

                // Count total rendered height (accounting for word wrap) to loop scroll
                const char* pp = s_changelog;
                while (*pp)
                {
                    const char* ls = pp;
                    int llen = 0;
                    while (*pp && *pp != '\n' && *pp != '\r') { ++pp; ++llen; }
                    if (*pp == '\r') ++pp;
                    if (*pp == '\n') ++pp;

                    if (llen == 0)
                    {
                        totalH += LINE_H;
                        continue;
                    }
                    // Count wrap rows for this source line
                    int pos2 = 0;

                    // Same normalization as render pass
                    while (pos2 < llen)
                    {
                        char c = ls[pos2];
                        if (c == '>' || c == '#' || c == ' ') ++pos2;
                        else break;
                    }
                    do
                    {
                        int fit = 0;
                        float w = 0.f;
                        while (pos2 + fit < llen)
                        {
                            // Measure next word + trailing whitespace
                            int wl = 0;
                            while (pos2 + fit + wl < llen && !IsWrapSpace(ls[pos2 + fit + wl])) ++wl;
                            int tl = wl;
                            while (pos2 + fit + tl < llen && IsWrapSpace(ls[pos2 + fit + tl])) ++tl;
                            char temp[64];
                            int cp = tl < 63 ? tl : 63;
                            for (int i = 0; i < cp; ++i) temp[i] = ls[pos2 + fit + i];
                            temp[cp] = '\0';
                            float tw = TW(temp, 1.0f);
                            if (w + tw > maxW) break;
                            w += tw;
                            fit += tl;
                        }
                        if (fit == 0) fit = 1;
                        totalH += LINE_H;
                        pos2 += fit;
                        while (pos2 < llen && IsWrapSpace(ls[pos2])) ++pos2;
                    } while (pos2 < llen);
                }
                if (s_changelogScroll > totalH) s_changelogScroll = 0.f;

                // Draw each line with word wrap clipped to frame
                while (*p)
                {
                    // Collect one source line (up to \n)
                    const char* lineStart = p;
                    int len = 0;
                    while (*p && *p != '\n' && *p != '\r') { ++p; ++len; }
                    if (*p == '\r') ++p;
                    if (*p == '\n') ++p;

                    // Word-wrap the source line into display rows
                    int pos = 0;

                    // Strip all leading markers and spaces
                    while (pos < len)
                    {
                        char c = lineStart[pos];
                        if (c == '>' || c == '#' || c == ' ') ++pos;
                        else break;
                    }
                    do
                    {
                        // Find how many complete words fit within maxW
                        int fit = 0;
                        float w = 0.f;
                        while (pos + fit < len)
                        {
                            int wl = 0;
                            while (pos + fit + wl < len && !IsWrapSpace(lineStart[pos + fit + wl])) ++wl;
                            int tl = wl;
                            while (pos + fit + tl < len && IsWrapSpace(lineStart[pos + fit + tl])) ++tl;
                            char temp[64];
                            int cp = tl < 63 ? tl : 63;
                            for (int i = 0; i < cp; ++i) temp[i] = lineStart[pos + fit + i];
                            temp[cp] = '\0';
                            float tw = TW(temp, 1.0f);
                            if (w + tw > maxW) break;
                            w += tw;
                            fit += tl;
                        }
                        if (fit == 0) fit = 1;

                        // Draw this segment left-aligned if visible
                        if (lineY + LINE_H > CT + LINE_H && lineY < CT + CH - 2.f)
                        {
                            char seg[128];
                            int copy = fit < 127 ? fit : 127;
                            for (int i = 0; i < copy; ++i) seg[i] = lineStart[pos + i];
                            seg[copy] = '\0';
                            DrawText(textX, lineY, seg, 1.0f, COL_WHITE);
                        }
                        lineY += LINE_H;

                        // Advance past this segment and any trailing whitespace
                        pos += fit;
                        while (pos < len && IsWrapSpace(lineStart[pos])) ++pos;

                    } while (pos < len);

                    // Empty source line still advances one row
                    if (len == 0) lineY += LINE_H;
                }
            }
            else
            {
                DrawText(CX + 6.f, CT + 6.f, "Fetching changelog...", 1.0f, COL_DIM);
            }
        }
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// Fetch /xbdiag/log.chg into s_changelog — blocking, called once when DNS resolved
static void FetchChangelog()
{
    if (s_changelogFetched) return;
    s_changelogFetched = true;

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return;

    u_long nb = 1; ioctlsocket(sock, FIONBIO, &nb);
    sockaddr_in sa; ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)k_httpPort);
    sa.sin_addr = s_serverAddr;
    int cr = connect(sock, (sockaddr*)&sa, sizeof(sa));
    if (cr == SOCKET_ERROR)
    {
        if (WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(sock); return; }
        fd_set wset; FD_ZERO(&wset); FD_SET(sock, &wset);
        TIMEVAL tv; tv.tv_sec = 2; tv.tv_usec = 0;
        if (select(0, NULL, &wset, NULL, &tv) <= 0) { closesocket(sock); return; }
    }
    nb = 0; ioctlsocket(sock, FIONBIO, &nb);
    int tmo = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));

    char req[256];
    StrCopy(req, sizeof(req), "GET /xbdiag/log.chg HTTP/1.0\r\nHost: ");
    AppendStr(req, sizeof(req), k_host);
    AppendStr(req, sizeof(req), "\r\nConnection: close\r\n\r\n");
    send(sock, req, StrLen(req), 0);

    char hdrBuf[2048]; int hdrLen = 0;
    while (hdrLen < (int)sizeof(hdrBuf) - 1)
    {
        int n = recv(sock, hdrBuf + hdrLen, (int)sizeof(hdrBuf) - 1 - hdrLen, 0);
        if (n <= 0) break;
        hdrLen += n; hdrBuf[hdrLen] = '\0';
        if (FindHeaderEnd(hdrBuf, hdrLen) >= 0) break;
    }
    int bodyStart = FindHeaderEnd(hdrBuf, hdrLen);
    if (bodyStart < 0 || GetHttpStatus(hdrBuf, hdrLen) != 200) { closesocket(sock); return; }

    // Copy body overflow
    int overflow = hdrLen - bodyStart;
    if (overflow > 0)
    {
        int copy = overflow < (int)sizeof(s_changelog) - 1 ? overflow : (int)sizeof(s_changelog) - 1;
        for (int i = 0; i < copy; ++i) s_changelog[i] = hdrBuf[bodyStart + i];
        s_changelogLen = copy;
    }

    // Read rest
    char tmp[512];
    while (s_changelogLen < (int)sizeof(s_changelog) - 1)
    {
        int n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        int space = (int)sizeof(s_changelog) - 1 - s_changelogLen;
        int copy = n < space ? n : space;
        for (int i = 0; i < copy; ++i) s_changelog[s_changelogLen + i] = tmp[i];
        s_changelogLen += copy;
    }
    s_changelog[s_changelogLen] = '\0';
    closesocket(sock);

    // Sanitize UTF-8 punctuation to ASCII before reflow/render
    Utf8ToAsciiInPlace(s_changelog, &s_changelogLen);

    // Reflow: join consecutive non-blank lines into single lines so the
    // renderer's word-wrap can fill the full frame width. Two consecutive
    // newlines (blank line) are preserved as paragraph breaks.
    {
        static char reflowed[4096];
        int ri = 0;
        const char* src = s_changelog;
        while (*src && ri < (int)sizeof(reflowed) - 2)
        {
            if (*src == '\r') { ++src; continue; }
            if (*src == '\n')
            {
                ++src;
                if (*src == '\r') ++src;

                // Count consecutive newlines to detect paragraph breaks
                int nlCount = 1;
                while (*src == '\n' || *src == '\r')
                {
                    if (*src == '\n') ++nlCount;
                    ++src;
                }

                if (nlCount >= 2)
                {
                    reflowed[ri++] = '\n';
                    reflowed[ri++] = '\n';
                }
                else
                {
                    // Single newline — join as space
                    if (ri > 0 && reflowed[ri - 1] != ' ' && reflowed[ri - 1] != '\n')
                        reflowed[ri++] = ' ';
                }
                continue;
            }
            else
            {
                reflowed[ri++] = *src++;
            }
        }
        reflowed[ri] = '\0';
        for (int i = 0; i <= ri; ++i) s_changelog[i] = reflowed[i];
        s_changelogLen = ri;
    }

    // Collapse multiple consecutive whitespace (spaces/tabs) into one space.
    // Tabs are normalized to spaces first.
    {
        static char cleaned[4096];
        int ci = 0;
        bool lastSpace = false;
        for (int i = 0; s_changelog[i] && ci < (int)sizeof(cleaned) - 1; ++i)
        {
            char c = s_changelog[i];
            if (c == '\t') c = ' ';
            if (IsWrapSpace(c))
            {
                if (!lastSpace) { cleaned[ci++] = ' '; lastSpace = true; }
            }
            else
            {
                cleaned[ci++] = c;
                lastSpace = (c == '\n');
            }
        }
        cleaned[ci] = '\0';
        for (int i = 0; i <= ci; ++i) s_changelog[i] = cleaned[i];
        s_changelogLen = ci;
    }

    // Strip leading heading line ("XbDiag Changelog" or similar) — the frame
    // already has a CHANGE LOG title so the first # heading is redundant.
    {
        char* p = s_changelog;
        // Skip past the first newline-terminated line if it starts with '#' or
        // is the known title string, then skip any blank lines after it.
        while (*p && *p != '\n') ++p;
        if (*p == '\n') ++p;
        while (*p == '\n') ++p;
        if (p > s_changelog)
        {
            int newLen = s_changelogLen - (int)(p - s_changelog);
            for (int i = 0; i <= newLen; ++i) s_changelog[i] = p[i];
            s_changelogLen = newLen;
        }
    }
}

// ============================================================================
// Blocking file download
// Mirrors xbox_update.cpp HttpDownloadFile:
//   - Non-blocking connect with select() 5s timeout
//   - Switch to blocking with SO_RCVTIMEO/SO_SNDTIMEO 10s
//   - Separate header buffer, write body overflow immediately
//   - FlushFileBuffers before CloseHandle
// Returns true on success. Does NOT call SetError.
// ============================================================================

static bool DoDownload(const char* path, const char* dest,
    const DiagLogo& logo, bool showProgress)
{
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;

    u_long nb = 1; ioctlsocket(sock, FIONBIO, &nb);
    sockaddr_in sa; ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)k_httpPort);
    sa.sin_addr = s_serverAddr;
    int cr = connect(sock, (sockaddr*)&sa, sizeof(sa));
    if (cr == SOCKET_ERROR)
    {
        if (WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(sock); return false; }
        fd_set wset; FD_ZERO(&wset); FD_SET(sock, &wset);
        TIMEVAL tv; tv.tv_sec = 5; tv.tv_usec = 0;
        if (select(0, NULL, &wset, NULL, &tv) <= 0) { closesocket(sock); return false; }
    }

    nb = 0; ioctlsocket(sock, FIONBIO, &nb);
    int tmo = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));

    char req[256];
    StrCopy(req, sizeof(req), "GET ");
    AppendStr(req, sizeof(req), path);
    AppendStr(req, sizeof(req), " HTTP/1.0\r\nHost: ");
    AppendStr(req, sizeof(req), k_host);
    AppendStr(req, sizeof(req), "\r\nConnection: close\r\n\r\n");
    if (send(sock, req, StrLen(req), 0) <= 0) { closesocket(sock); return false; }

    char hdrBuf[2048]; int hdrLen = 0;
    while (hdrLen < (int)sizeof(hdrBuf) - 1)
    {
        int n = recv(sock, hdrBuf + hdrLen, (int)sizeof(hdrBuf) - 1 - hdrLen, 0);
        if (n <= 0) { closesocket(sock); return false; }
        hdrLen += n; hdrBuf[hdrLen] = '\0';
        if (FindHeaderEnd(hdrBuf, hdrLen) >= 0) break;
    }

    int bodyStart = FindHeaderEnd(hdrBuf, hdrLen);
    if (bodyStart < 0) { closesocket(sock); return false; }
    if (GetHttpStatus(hdrBuf, hdrLen) != 200) { closesocket(sock); return false; }

    DWORD cl = ParseContentLength(hdrBuf, bodyStart);
    if (showProgress) { s_xbeTotal = cl; s_xbeReceived = 0; }

    HANDLE hf = CreateFile(dest, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { closesocket(sock); return false; }

    int overflow = hdrLen - bodyStart;
    DWORD totalRecv = 0;
    if (overflow > 0)
    {
        DWORD wr = 0; WriteFile(hf, hdrBuf + bodyStart, (DWORD)overflow, &wr, NULL);
        totalRecv += wr;
        if (showProgress) s_xbeReceived = totalRecv;
    }

    char dlBuf[4096];
    while (true)
    {
        if (showProgress) Render(logo);
        int n = recv(sock, dlBuf, sizeof(dlBuf), 0);
        if (n <= 0) break;
        DWORD wr = 0; WriteFile(hf, dlBuf, (DWORD)n, &wr, NULL);
        totalRecv += wr;
        if (showProgress) s_xbeReceived = totalRecv;
        if (cl > 0 && totalRecv >= cl) break;
    }

    FlushFileBuffers(hf);
    CloseHandle(hf);
    closesocket(sock);

    if (totalRecv == 0) { DeleteFile(dest); return false; }
    return true;
}

// ============================================================================
// Tick
// ============================================================================

void Update_Tick(const DiagLogo& logo)
{
    WORD cur = GetButtons();

    if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
    {
        CloseSocket(); CloseFile();
        if (s_dns) { XNetDnsRelease(s_dns); s_dns = NULL; }
        s_state = UPST_IDLE; s_prevBtns = cur;
        RequestState(MSTATE_MENU); return;
    }

    if (EdgeDown(cur, s_prevBtns, BTN_A))
    {
        switch (s_state)
        {
        case UPST_IDLE:
        case UPST_ERROR:
            CloseSocket();
            if (s_dns) { XNetDnsRelease(s_dns); s_dns = NULL; }
            s_recvLen = 0;
            ZeroMemory(s_recvBuf, sizeof(s_recvBuf));
            ZeroMemory(s_remoteVer, sizeof(s_remoteVer));
            ZeroMemory(s_errorMsg, sizeof(s_errorMsg));
            s_xbeTotal = s_xbeReceived = 0;
            ReadLocalVer();
            NetEnsure();
            s_netInitStart = GetTickCount();
            s_state = UPST_NET_INIT;
            break;

        case UPST_UP_TO_DATE:
            CloseSocket();
            if (s_dns) { XNetDnsRelease(s_dns); s_dns = NULL; }
            s_recvLen = 0;
            ZeroMemory(s_recvBuf, sizeof(s_recvBuf));
            ZeroMemory(s_remoteVer, sizeof(s_remoteVer));
            s_netInitStart = GetTickCount();
            s_state = UPST_NET_INIT;
            break;

        case UPST_AVAIL:
        {
            FE_MU_MountAll();

            // Build base dir from k_xbeDest e.g. "D:\"
            char base[64]; StrCopy(base, sizeof(base), k_xbeDest);
            int blen = StrLen(base);
            for (int i = blen - 1; i >= 0; --i)
                if (base[i] == '\\') { base[i + 1] = '\0'; break; }

            // Step 1: download default.xbe
            s_state = UPST_RECV_XBE;
            char xbeDest[128]; StrCopy(xbeDest, sizeof(xbeDest), base); AppendStr(xbeDest, sizeof(xbeDest), "default.xbe");
            if (!DoDownload("/xbdiag/default.xbe", xbeDest, logo, true))
            {
                SetError("dl failed: default.xbe"); break;
            }

            // Step 2: wait 5 seconds, pump render
            s_state = UPST_RECV_VER2;
            DWORD waitStart = GetTickCount();
            while (GetTickCount() - waitStart < 5000)
                Render(logo);

            // Step 3: download XbDiag.ver
            char verDest[128]; StrCopy(verDest, sizeof(verDest), base); AppendStr(verDest, sizeof(verDest), "XbDiag.ver");
            if (!DoDownload("/xbdiag/XbDiag.ver", verDest, logo, false))
            {
                SetError("dl failed: XbDiag.ver"); break;
            }

            s_state = UPST_WRITE_DONE;
            break;
        }

        case UPST_WRITE_DONE:
        {
            LAUNCH_DATA ld; ZeroMemory(&ld, sizeof(ld));
            XLaunchNewImage(NULL, &ld);
            while (true) {}
        }

        default: break;
        }
    }

    s_prevBtns = cur;

    // State machine advance
    switch (s_state)
    {
    case UPST_NET_INIT:
    {
        XNADDR xna; ZeroMemory(&xna, sizeof(xna));
        DWORD st = XNetGetTitleXnAddr(&xna);
        if (st == XNET_GET_XNADDR_PENDING)
        {
            if (GetTickCount() - s_netInitStart > 5000) SetError("No network link"); break;
        }
        if ((st & XNET_GET_XNADDR_NONE) || xna.ina.s_addr == 0)
        {
            SetError("No network link"); break;
        }
        int dr = XNetDnsLookup(k_host, NULL, &s_dns);
        if (dr != 0 || !s_dns) { SetError("DNS lookup failed"); break; }
        s_state = UPST_DNS; break;
    }
    case UPST_DNS:
    {
        if (!s_dns) { SetError("DNS handle null"); break; }
        if (s_dns->iStatus == WSAEINPROGRESS) break;
        if (s_dns->iStatus != 0)
        {
            XNetDnsRelease(s_dns); s_dns = NULL; SetError("DNS resolution failed"); break;
        }
        s_serverAddr = s_dns->aina[0];
        XNetDnsRelease(s_dns); s_dns = NULL;
        BeginConnect(k_httpPort, UPST_CONNECT_VER); break;
    }
    case UPST_CONNECT_VER:
    {
        if (!PollConnect()) break;
        s_recvLen = 0; ZeroMemory(s_recvBuf, sizeof(s_recvBuf));
        if (!SendGet(k_verPath)) break;
        s_state = UPST_RECV_VER; break;
    }
    case UPST_RECV_VER:
    {
        int space = (int)sizeof(s_recvBuf) - s_recvLen - 1;
        if (space <= 0) { s_state = UPST_COMPARE; break; }
        int n = recv(s_sock, s_recvBuf + s_recvLen, space, 0);
        if (n > 0) { s_recvLen += n; s_recvBuf[s_recvLen] = '\0'; }
        else if (n == 0) { CloseSocket(); s_state = UPST_COMPARE; }
        else { if (WSAGetLastError() != WSAEWOULDBLOCK) SetError("Connection lost"); }
        break;
    }
    case UPST_COMPARE:
    {
        int status = GetHttpStatus(s_recvBuf, s_recvLen);
        if (status != 200)
        {
            char msg[40]; StrCopy(msg, sizeof(msg), "HTTP error ");
            char c[8]; IntToStr(status, c, sizeof(c)); AppendStr(msg, sizeof(msg), c);
            SetError(msg); break;
        }
        DWORD cl = 0; const char* body = FindBody(s_recvBuf, s_recvLen, &cl);
        if (!body) { SetError("Malformed HTTP response"); break; }
        StrCopy(s_remoteVer, sizeof(s_remoteVer), body); StripWhitespace(s_remoteVer);
        if (s_remoteVer[0] == '\0') { SetError("Empty version response"); break; }
        s_isNewer = (s_localVer[0] == '\0' || VerCmp(s_remoteVer, s_localVer) > 0);
        s_state = s_isNewer ? UPST_AVAIL : UPST_UP_TO_DATE;
        break;
    }
    case UPST_UP_TO_DATE:
    case UPST_AVAIL:
    case UPST_RECV_XBE:
    case UPST_RECV_VER2:
    case UPST_WRITE_DONE:
    case UPST_ERROR:
    case UPST_IDLE:
    default:
        break;
    }

    Render(logo);
}