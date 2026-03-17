// Update.cpp
// XbDiag - Software update checker and downloader.
//
// State machine (s_state):
//   UPST_IDLE          - waiting for user to press [A], or auto-entry from boot check
//   UPST_NET_INIT      - bring up XNet + Winsock, poll for IP
//   UPST_DNS           - XNetDnsLookup in flight for raw.githubusercontent.com
//   UPST_CONNECT       - non-blocking TCP connect to resolved IP:443... but Xbox
//                        has no TLS stack, so we use port 80 via the raw CDN path.
//                        GitHub redirects HTTPS; we use the raw CDN IP directly
//                        on port 80 with the correct Host header. See note below.
//   UPST_SEND_VER      - send HTTP GET for XbDiag.ver
//   UPST_RECV_VER      - read HTTP response, parse version string
//   UPST_COMPARE       - compare remote vs local
//   UPST_UP_TO_DATE    - no update needed, display result
//   UPST_AVAIL         - update available, prompt [A] to download
//   UPST_SEND_XBE      - send HTTP GET for XbDiag.xbe
//   UPST_RECV_XBE      - streaming download with progress bar
//   UPST_WRITE_DONE    - file write complete, prompt relaunch
//   UPST_ERROR         - error with message string, [B] to go back
//
// HTTP note:
//   raw.githubusercontent.com serves plain HTTP on port 80 at the CDN level.
//   We resolve the hostname via XNetDnsLookup and issue a plain HTTP/1.0 GET
//   with the correct Host header. This avoids any TLS requirement.
//   HTTP/1.0 (not 1.1) because it closes the connection after the response
//   body, giving us a clean EOF signal without chunked encoding or keep-alive.
//
// Boot check:
//   Update_StartBootCheck() is called from main() before the game loop starts.
//   It runs the DNS + connect + ver fetch synchronously in small polling steps
//   so it doesn't block the frame pump. Once Update_IsCheckComplete() returns
//   true, main() checks Update_BootFoundUpdate() and transitions to STATE_UPDATE
//   if a newer version was found.
//
// File write:
//   The download streams directly into D:\XbDiag.xbe. The running XBE is
//   memory-mapped by the kernel at load time; overwriting the file on disk
//   while it executes is safe on Xbox (the kernel holds a file handle but
//   the image is already in RAM). On relaunch the new binary is loaded fresh.
//
// RXDK constraints observed throughout:
//   - No sprintf / sscanf / strlen — use DiagCommon helpers
//   - No inline asm (not needed here)
//   - File-scope statics only for all persistent state
//   - Ftoi() for float-to-int
//   - All sockets non-blocking (FIONBIO), polled once per Tick()

#include "Update.h"
#include "font.h"
#include "input.h"
#include <xtl.h>
#include <winsockx.h>

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

// ============================================================================
// Version constants
// ============================================================================

// THE single authoritative local version string.
// AboutScreen calls Update_GetLocalVersion() — do not hardcode it elsewhere.
static const char* k_localVersion = "1.0.2 Beta";

// Remote URLs
// Host:  raw.githubusercontent.com
// Paths: /Darkone83/XbDiag/main/xbe/XbDiag.ver
//        /Darkone83/XbDiag/main/xbe/XbDiag.xbe
static const char* k_host = "raw.githubusercontent.com";
static const char* k_verPath = "/Darkone83/XbDiag/main/xbe/XbDiag.ver";
static const char* k_xbePath = "/Darkone83/XbDiag/main/xbe/XbDiag.xbe";
static const char* k_xbeDest = "D:\\XbDiag.xbe";
static const int   k_httpPort = 80;

// ============================================================================
// State enum
// ============================================================================

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
    UPST_CONNECT_XBE,
    UPST_SEND_XBE,
    UPST_RECV_XBE,
    UPST_WRITE_DONE,
    UPST_ERROR,
};

// ============================================================================
// Module-level state
// ============================================================================

static UpdateState s_state = UPST_IDLE;
static WORD        s_prevBtns = 0;

// Network
static bool        s_netUp = false;   // XNet + Winsock initialised
static XNDNS* s_dns = NULL;    // XNetDnsLookup handle
static IN_ADDR     s_serverAddr;             // resolved IP
static SOCKET      s_sock = INVALID_SOCKET;

// HTTP receive buffer — large enough for the full .ver response header+body
static char        s_recvBuf[4096];
static int         s_recvLen = 0;

// Version result
static char        s_remoteVer[32];  // stripped version string from server
static bool        s_isNewer = false;

// Download progress
static HANDLE      s_hFile = INVALID_HANDLE_VALUE;
static DWORD       s_xbeTotal = 0;   // Content-Length parsed from header
static DWORD       s_xbeReceived = 0;   // bytes written so far

// Error message
static char        s_errorMsg[80];

// Boot-check results (set from boot-check path, read by main.cpp)
static bool        s_bootCheckDone = false;
static bool        s_bootFoundUpdate = false;

// Timer for NET_INIT polling
static DWORD       s_netInitStart = 0;

// ============================================================================
// String helpers (no sprintf / sscanf / strlen — RXDK rules)
// ============================================================================

// Compare two version strings of the form "MAJOR.MINOR.PATCH[ suffix]".
// Strips any trailing non-digit suffix (space, letters) before parsing.
// Returns -1 if a < b, 0 if equal, 1 if a > b.
static int VerCmp(const char* a, const char* b)
{
    // Parse up to 3 dot-separated integer fields from a version string.
    // Stops at end-of-string, space, or any char that's not a digit or dot.
    struct Parts { int v[3]; };

    auto Parse = [](const char* s, Parts& p)
        {
            p.v[0] = p.v[1] = p.v[2] = 0;
            int field = 0;
            for (int i = 0; s[i] != '\0' && field < 3; ++i)
            {
                char c = s[i];
                if (c >= '0' && c <= '9')
                    p.v[field] = p.v[field] * 10 + (int)(c - '0');
                else if (c == '.')
                    ++field;
                else
                    break;  // space / suffix — stop
            }
        };

    Parts pa, pb;
    Parse(a, pa);
    Parse(b, pb);

    for (int i = 0; i < 3; ++i)
    {
        if (pa.v[i] < pb.v[i]) return -1;
        if (pa.v[i] > pb.v[i]) return  1;
    }
    return 0;
}

// Strip leading/trailing whitespace and \r\n in-place (modifies buf).
static void StripWhitespace(char* buf)
{
    int len = StrLen(buf);

    // Trim trailing
    while (len > 0)
    {
        char c = buf[len - 1];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
            buf[--len] = '\0';
        else
            break;
    }

    // Trim leading
    int start = 0;
    while (buf[start] == ' ' || buf[start] == '\t') ++start;
    if (start > 0)
    {
        int i = 0;
        while (buf[start + i]) { buf[i] = buf[start + i]; ++i; }
        buf[i] = '\0';
    }
}

// Append a C string to a fixed buffer — same contract as StrCat2 but single source.
// (StrCat2 requires dst != src in our codebase, so we use this for appending to self)
static void AppendStr(char* dst, int dstLen, const char* src)
{
    int dlen = StrLen(dst);
    int slen = StrLen(src);
    int space = dstLen - dlen - 1;
    for (int i = 0; i < slen && i < space; ++i)
        dst[dlen + i] = src[i];
    dst[dlen + (slen < space ? slen : space)] = '\0';
}

// ============================================================================
// Network helpers
// ============================================================================

static void NetEnsure()
{
    if (s_netUp) return;

    XNetStartupParams xnsp;
    ZeroMemory(&xnsp, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    XNetStartup(&xnsp);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    s_netUp = true;
}

static void CloseSocket()
{
    if (s_sock != INVALID_SOCKET)
    {
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
    }
}

static void CloseFile()
{
    if (s_hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(s_hFile);
        s_hFile = INVALID_HANDLE_VALUE;
    }
}

// Set error state with a message
static void SetError(const char* msg)
{
    StrCopy(s_errorMsg, sizeof(s_errorMsg), msg);
    s_state = UPST_ERROR;
    CloseSocket();
    CloseFile();
    if (s_dns) { XNetDnsRelease(s_dns); s_dns = NULL; }
}

// Open a non-blocking TCP socket and kick off a connect to s_serverAddr:port.
// Transitions to nextState on WSAEWOULDBLOCK (connect in progress).
static bool BeginConnect(int port, UpdateState nextState)
{
    CloseSocket();

    s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_sock == INVALID_SOCKET)
    {
        SetError("socket() failed");
        return false;
    }

    u_long nb = 1;
    ioctlsocket(s_sock, FIONBIO, &nb);

    sockaddr_in sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    sa.sin_addr = s_serverAddr;

    int r = connect(s_sock, (sockaddr*)&sa, sizeof(sa));
    if (r == 0 || WSAGetLastError() == WSAEWOULDBLOCK)
    {
        s_state = nextState;
        return true;
    }

    SetError("connect() failed");
    return false;
}

// Poll a pending non-blocking connect using select() with zero timeout.
// Returns true if connected, false if still pending, calls SetError on failure.
static bool PollConnect()
{
    fd_set wfds, efds;
    FD_ZERO(&wfds); FD_SET(s_sock, &wfds);
    FD_ZERO(&efds); FD_SET(s_sock, &efds);
    TIMEVAL tv = { 0, 0 };

    int r = select(0, NULL, &wfds, &efds, &tv);
    if (r == SOCKET_ERROR) { SetError("select() failed"); return false; }
    if (FD_ISSET(s_sock, &efds)) { SetError("TCP connect refused"); return false; }
    return FD_ISSET(s_sock, &wfds) != 0;
}

// Build and send an HTTP/1.0 GET request for the given path.
static bool SendGet(const char* path)
{
    char req[256];
    StrCopy(req, sizeof(req), "GET ");
    AppendStr(req, sizeof(req), path);
    AppendStr(req, sizeof(req), " HTTP/1.0\r\nHost: ");
    AppendStr(req, sizeof(req), k_host);
    AppendStr(req, sizeof(req), "\r\nConnection: close\r\n\r\n");

    int total = StrLen(req);
    int sent = 0;
    while (sent < total)
    {
        int n = send(s_sock, req + sent, total - sent, 0);
        if (n == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) continue;  // retry same frame
            SetError("send() failed");
            return false;
        }
        sent += n;
    }
    return true;
}

// Find the HTTP body start (past \r\n\r\n) and parse Content-Length if present.
// Returns pointer into buf at start of body, or NULL if header not yet complete.
static const char* FindBody(const char* buf, int len, DWORD* outContentLength)
{
    *outContentLength = 0;

    // Find \r\n\r\n
    for (int i = 0; i < len - 3; ++i)
    {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
        {
            // Scan header lines for Content-Length
            for (int j = 0; j < i; ++j)
            {
                // Case-insensitive match for "content-length:"
                const char* needle = "Content-Length:";
                bool match = true;
                for (int k = 0; needle[k] && (j + k) < i; ++k)
                {
                    char hc = buf[j + k];
                    char nc = needle[k];
                    // tolower
                    if (hc >= 'A' && hc <= 'Z') hc = (char)(hc + 32);
                    if (nc >= 'A' && nc <= 'Z') nc = (char)(nc + 32);
                    if (hc != nc) { match = false; break; }
                }
                if (match)
                {
                    int pos = j + StrLen(needle);
                    while (buf[pos] == ' ') ++pos;
                    DWORD val = 0;
                    while (buf[pos] >= '0' && buf[pos] <= '9')
                        val = val * 10 + (DWORD)(buf[pos++] - '0');
                    *outContentLength = val;
                    break;
                }
            }
            return buf + i + 4;
        }
    }
    return NULL;
}

// ============================================================================
// Boot-check polling (called from Update_StartBootCheck / Update_IsCheckComplete)
// Runs the DNS + connect + ver-fetch inline but only advances one step per call
// so main.cpp can call it in a tight loop without fully blocking.
// ============================================================================

static bool s_bootCheckStarted = false;

static void BootCheckPoll()
{
    // This drives the state machine a single step per call.
    // main.cpp calls Update_IsCheckComplete() in the autorun countdown loop.

    switch (s_state)
    {
    case UPST_NET_INIT:
    {
        // Poll until we have an IP or timeout (3 seconds)
        XNADDR xna; ZeroMemory(&xna, sizeof(xna));
        DWORD st = XNetGetTitleXnAddr(&xna);
        if (st == XNET_GET_XNADDR_PENDING)
        {
            if (GetTickCount() - s_netInitStart > 3000)
                SetError("No network link");
            return;
        }
        if ((st & XNET_GET_XNADDR_NONE) || xna.ina.s_addr == 0)
        {
            SetError("No network link");
            return;
        }
        // Start DNS
        int dnsResult = XNetDnsLookup(k_host, NULL, &s_dns);
        if (dnsResult != 0 || !s_dns)
        {
            SetError("DNS lookup failed");
            return;
        }
        s_state = UPST_DNS;
        return;
    }

    case UPST_DNS:
    {
        if (!s_dns) { SetError("DNS handle null"); return; }
        if (s_dns->iStatus == WSAEINPROGRESS) return;  // still resolving
        if (s_dns->iStatus != 0)
        {
            XNetDnsRelease(s_dns); s_dns = NULL;
            SetError("DNS failed");
            return;
        }
        s_serverAddr = s_dns->aina[0];
        XNetDnsRelease(s_dns); s_dns = NULL;
        BeginConnect(k_httpPort, UPST_CONNECT_VER);
        return;
    }

    case UPST_CONNECT_VER:
    {
        if (!PollConnect()) return;
        s_recvLen = 0;
        ZeroMemory(s_recvBuf, sizeof(s_recvBuf));
        if (!SendGet(k_verPath)) return;
        s_state = UPST_RECV_VER;
        return;
    }

    case UPST_RECV_VER:
    {
        // Recv until connection closes or buffer full
        int space = (int)sizeof(s_recvBuf) - s_recvLen - 1;
        if (space <= 0) { s_state = UPST_COMPARE; return; }

        int n = recv(s_sock, s_recvBuf + s_recvLen, space, 0);
        if (n > 0)
        {
            s_recvLen += n;
            s_recvBuf[s_recvLen] = '\0';
        }
        else if (n == 0)
        {
            // Connection closed by server — response complete
            CloseSocket();
            s_state = UPST_COMPARE;
        }
        else
        {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK)
            {
                SetError("recv error");
            }
        }
        return;
    }

    case UPST_COMPARE:
    {
        DWORD cl = 0;
        const char* body = FindBody(s_recvBuf, s_recvLen, &cl);
        if (!body) { SetError("Bad HTTP response"); return; }

        StrCopy(s_remoteVer, sizeof(s_remoteVer), body);
        StripWhitespace(s_remoteVer);

        s_isNewer = (VerCmp(s_remoteVer, k_localVersion) > 0);
        s_bootFoundUpdate = s_isNewer;
        s_bootCheckDone = true;

        s_state = s_isNewer ? UPST_AVAIL : UPST_UP_TO_DATE;
        return;
    }

    default:
        // Any terminal state (UP_TO_DATE, AVAIL, ERROR) — check is done
        s_bootCheckDone = true;
        return;
    }
}

// ============================================================================
// Public boot-check API
// ============================================================================

void Update_StartBootCheck()
{
    if (s_bootCheckStarted) return;
    s_bootCheckStarted = true;
    s_bootCheckDone = false;
    s_bootFoundUpdate = false;

    ZeroMemory(s_remoteVer, sizeof(s_remoteVer));
    ZeroMemory(s_errorMsg, sizeof(s_errorMsg));
    s_recvLen = 0;

    NetEnsure();

    s_netInitStart = GetTickCount();
    s_state = UPST_NET_INIT;
}

bool Update_IsCheckComplete()
{
    if (!s_bootCheckStarted) return true;  // never started = trivially done

    // Drive the state machine forward one step
    if (!s_bootCheckDone)
        BootCheckPoll();

    return s_bootCheckDone;
}

bool Update_BootFoundUpdate()
{
    return s_bootFoundUpdate;
}

const char* Update_GetLocalVersion()
{
    return k_localVersion;
}

// ============================================================================
// OnEnter
// Called when main.cpp transitions to STATE_UPDATE.
// If the boot check already ran and has results we skip straight to the
// appropriate display state rather than re-running the full check.
// ============================================================================

void Update_OnEnter()
{
    s_prevBtns = GetButtons();
    ZeroMemory(s_errorMsg, sizeof(s_errorMsg));
    CloseFile();

    // If the boot check already resolved to AVAIL or UP_TO_DATE, stay there.
    // The user landed here either because main.cpp detected s_bootFoundUpdate
    // and jumped straight in, or they navigated here manually.
    if (s_state == UPST_AVAIL || s_state == UPST_UP_TO_DATE ||
        s_state == UPST_WRITE_DONE)
    {
        return;
    }

    // If we're in an error state from boot check, reset for a fresh manual check.
    if (s_state == UPST_ERROR || s_state == UPST_IDLE)
    {
        s_state = UPST_IDLE;
        s_recvLen = 0;
        ZeroMemory(s_remoteVer, sizeof(s_remoteVer));
        s_xbeTotal = 0;
        s_xbeReceived = 0;
        return;
    }

    // All other in-progress states: leave them running — the Tick will continue.
}

// ============================================================================
// Render helpers
// ============================================================================

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

// Draw a progress bar in the style of the rest of XbDiag.
// x,y = top-left corner, w = total width, h = height, frac = 0.0..1.0
static void DrawProgressBar(float x, float y, float w, float h, float frac)
{
    // Background
    FillRect(x, y, x + w, y + h, D3DCOLOR_XRGB(20, 24, 52));
    // Border
    HLine(y, x, x + w, COL_BORDER);
    HLine(y + h, x, x + w, COL_BORDER);
    VLine(x, y, y + h, COL_BORDER);
    VLine(x + w, y, y + h, COL_BORDER);
    // Fill — clamp frac
    if (frac < 0.f) frac = 0.f;
    if (frac > 1.f) frac = 1.f;
    float fillW = (w - 2.f) * frac;
    if (fillW > 0.f)
        FillRectGrad(x + 1.f, y + 1.f, x + 1.f + fillW, y + h - 1.f,
            D3DCOLOR_XRGB(40, 120, 220),
            D3DCOLOR_XRGB(20, 80, 160));
    // Percentage text
    int pct = Ftoi(frac * 100.f);
    char pctBuf[8];
    IntToStr(pct, pctBuf, sizeof(pctBuf));
    char pctStr[12];
    StrCopy(pctStr, sizeof(pctStr), pctBuf);
    AppendStr(pctStr, sizeof(pctStr), "%");
    float tw = TW(pctStr, 1.2f);
    DrawText(x + (w - tw) * 0.5f, y + (h - LINE_H) * 0.5f, pctStr, 1.2f, COL_WHITE);
}

// ============================================================================
// Render
// ============================================================================

static void Render(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    // Build hint string based on state
    const char* hint = "[B] Back";
    if (s_state == UPST_IDLE)
        hint = "[A] Check for update    [B] Back";
    else if (s_state == UPST_AVAIL)
        hint = "[A] Download update    [B] Back";
    else if (s_state == UPST_WRITE_DONE)
        hint = "[A] Relaunch now    [B] Back to menu";
    else if (s_state == UPST_UP_TO_DATE)
        hint = "[A] Re-check    [B] Back";
    else if (s_state == UPST_ERROR)
        hint = "[A] Retry    [B] Back";

    DrawPageChrome(logo, "SOFTWARE UPDATE", hint);

    const float LX = LM;
    const float VX = LM + 130.f;
    const float LH = LINE_H + 2.f;
    float y = CONTENT_Y + 16.f;

    // ---- Local version row ----
    DrawText(LX, y, "LOCAL VERSION :", 1.2f, COL_GRAY);
    DrawText(VX, y, k_localVersion, 1.2f, COL_CYAN);
    y += LH;

    // ---- Remote version row ----
    DrawText(LX, y, "REMOTE VERSION:", 1.2f, COL_GRAY);
    {
        const char* remStr = s_remoteVer[0] ? s_remoteVer : "---";
        DWORD remCol = COL_DIM;
        if (s_remoteVer[0])
            remCol = s_isNewer ? COL_YELLOW : COL_GREEN;
        DrawText(VX, y, remStr, 1.2f, remCol);
    }
    y += LH * 2.f;

    // ---- Status line ----
    DrawText(LX, y, "STATUS        :", 1.2f, COL_GRAY);
    {
        const char* statusStr = "";
        DWORD statusCol = COL_WHITE;

        switch (s_state)
        {
        case UPST_IDLE:
            statusStr = "Press [A] to check for updates";
            statusCol = COL_DIM;
            break;
        case UPST_NET_INIT:
            statusStr = "Waiting for network link...";
            statusCol = COL_YELLOW;
            break;
        case UPST_DNS:
            statusStr = "Resolving host...";
            statusCol = COL_YELLOW;
            break;
        case UPST_CONNECT_VER:
        case UPST_CONNECT_XBE:
            statusStr = "Connecting...";
            statusCol = COL_YELLOW;
            break;
        case UPST_SEND_VER:
        case UPST_RECV_VER:
            statusStr = "Fetching version info...";
            statusCol = COL_YELLOW;
            break;
        case UPST_COMPARE:
            statusStr = "Comparing versions...";
            statusCol = COL_YELLOW;
            break;
        case UPST_UP_TO_DATE:
            statusStr = "XbDiag is up to date";
            statusCol = COL_GREEN;
            break;
        case UPST_AVAIL:
            statusStr = "Update available!  Press [A] to download";
            statusCol = COL_YELLOW;
            break;
        case UPST_SEND_XBE:
        case UPST_RECV_XBE:
            statusStr = "Downloading update...";
            statusCol = COL_CYAN;
            break;
        case UPST_WRITE_DONE:
            statusStr = "Download complete!  Press [A] to relaunch";
            statusCol = COL_GREEN;
            break;
        case UPST_ERROR:
            statusStr = s_errorMsg;
            statusCol = COL_RED;
            break;
        default:
            break;
        }
        DrawText(VX, y, statusStr, 1.2f, statusCol);
    }
    y += LH * 2.f;

    // ---- Progress bar (only shown during XBE download) ----
    if (s_state == UPST_RECV_XBE || s_state == UPST_WRITE_DONE)
    {
        const float BAR_W = SW - LM * 2.f;
        const float BAR_H = 22.f;

        DrawText(LX, y, "PROGRESS      :", 1.2f, COL_GRAY);
        y += LH;

        float frac = 0.f;
        if (s_xbeTotal > 0)
            frac = (float)s_xbeReceived / (float)s_xbeTotal;
        else if (s_state == UPST_WRITE_DONE)
            frac = 1.f;

        DrawProgressBar(LX, y, BAR_W, BAR_H, frac);
        y += BAR_H + 8.f;

        // Byte count
        char rcvBuf[16]; IntToStr((int)(s_xbeReceived / 1024), rcvBuf, sizeof(rcvBuf));
        char totBuf[16]; IntToStr((int)(s_xbeTotal / 1024), totBuf, sizeof(totBuf));
        char byteStr[40];
        StrCopy(byteStr, sizeof(byteStr), rcvBuf);
        AppendStr(byteStr, sizeof(byteStr), " KB");
        if (s_xbeTotal > 0)
        {
            AppendStr(byteStr, sizeof(byteStr), " / ");
            AppendStr(byteStr, sizeof(byteStr), totBuf);
            AppendStr(byteStr, sizeof(byteStr), " KB");
        }
        DrawText(LX, y, byteStr, 1.1f, COL_DIM);
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// Tick — state machine advance + render
// ============================================================================

void Update_Tick(const DiagLogo& logo)
{
    WORD cur = GetButtons();

    // ---- Input handling ----
    if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
    {
        // Clean up and go back to menu
        CloseSocket();
        CloseFile();
        if (s_dns) { XNetDnsRelease(s_dns); s_dns = NULL; }
        // Reset to IDLE so re-entry gives a fresh check option
        s_state = UPST_IDLE;
        s_prevBtns = cur;
        RequestState(MSTATE_MENU);
        return;
    }

    if (EdgeDown(cur, s_prevBtns, BTN_A))
    {
        switch (s_state)
        {
        case UPST_IDLE:
        case UPST_ERROR:
            // Start / retry a fresh check
            CloseSocket();
            if (s_dns) { XNetDnsRelease(s_dns); s_dns = NULL; }
            s_recvLen = 0;
            ZeroMemory(s_recvBuf, sizeof(s_recvBuf));
            ZeroMemory(s_remoteVer, sizeof(s_remoteVer));
            ZeroMemory(s_errorMsg, sizeof(s_errorMsg));
            s_xbeTotal = s_xbeReceived = 0;
            NetEnsure();
            s_netInitStart = GetTickCount();
            s_state = UPST_NET_INIT;
            break;

        case UPST_UP_TO_DATE:
            // Re-check
            CloseSocket();
            if (s_dns) { XNetDnsRelease(s_dns); s_dns = NULL; }
            s_recvLen = 0;
            ZeroMemory(s_recvBuf, sizeof(s_recvBuf));
            ZeroMemory(s_remoteVer, sizeof(s_remoteVer));
            s_netInitStart = GetTickCount();
            s_state = UPST_NET_INIT;
            break;

        case UPST_AVAIL:
            // Begin XBE download — re-resolve DNS, reconnect fresh socket
            CloseSocket();
            if (s_dns) { XNetDnsRelease(s_dns); s_dns = NULL; }
            s_recvLen = 0;
            ZeroMemory(s_recvBuf, sizeof(s_recvBuf));
            s_xbeTotal = s_xbeReceived = 0;
            CloseFile();
            // Open destination file
            s_hFile = CreateFileA(k_xbeDest, GENERIC_WRITE, 0, NULL,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (s_hFile == INVALID_HANDLE_VALUE)
            {
                SetError("Cannot open D:\\XbDiag.xbe for write");
                break;
            }
            // Reconnect to server
            BeginConnect(k_httpPort, UPST_CONNECT_XBE);
            break;

        case UPST_WRITE_DONE:
            // Relaunch the updated XBE
            CloseFile();
            {
                // XLaunchNewImage launches a local XBE by path.
                // NULL second arg = no additional parameters.
                LAUNCH_DATA ld; ZeroMemory(&ld, sizeof(ld));
                XLaunchNewImage(k_xbeDest, &ld);
                // Should not return — idle loop just in case
                while (true) {}
            }
            break;

        default:
            break;
        }
    }

    s_prevBtns = cur;

    // ---- State machine advance (one step per frame) ----
    switch (s_state)
    {
        // ---- NET_INIT: poll for IP ----
    case UPST_NET_INIT:
    {
        XNADDR xna; ZeroMemory(&xna, sizeof(xna));
        DWORD st = XNetGetTitleXnAddr(&xna);
        if (st == XNET_GET_XNADDR_PENDING)
        {
            if (GetTickCount() - s_netInitStart > 5000)
                SetError("No network link");
            break;
        }
        if ((st & XNET_GET_XNADDR_NONE) || xna.ina.s_addr == 0)
        {
            SetError("No network link");
            break;
        }
        int dnsResult = XNetDnsLookup(k_host, NULL, &s_dns);
        if (dnsResult != 0 || !s_dns)
        {
            SetError("DNS lookup failed");
            break;
        }
        s_state = UPST_DNS;
        break;
    }

    // ---- DNS: poll for resolution ----
    case UPST_DNS:
    {
        if (!s_dns) { SetError("DNS handle null"); break; }
        if (s_dns->iStatus == WSAEINPROGRESS) break;
        if (s_dns->iStatus != 0)
        {
            XNetDnsRelease(s_dns); s_dns = NULL;
            SetError("DNS resolution failed");
            break;
        }
        s_serverAddr = s_dns->aina[0];
        XNetDnsRelease(s_dns); s_dns = NULL;
        BeginConnect(k_httpPort, UPST_CONNECT_VER);
        break;
    }

    // ---- CONNECT_VER: poll connect, then send GET for .ver ----
    case UPST_CONNECT_VER:
    {
        if (!PollConnect()) break;
        s_recvLen = 0;
        ZeroMemory(s_recvBuf, sizeof(s_recvBuf));
        if (!SendGet(k_verPath)) break;
        s_state = UPST_RECV_VER;
        break;
    }

    // ---- RECV_VER: accumulate response until EOF ----
    case UPST_RECV_VER:
    {
        int space = (int)sizeof(s_recvBuf) - s_recvLen - 1;
        if (space <= 0) { s_state = UPST_COMPARE; break; }

        int n = recv(s_sock, s_recvBuf + s_recvLen, space, 0);
        if (n > 0)
        {
            s_recvLen += n;
            s_recvBuf[s_recvLen] = '\0';
        }
        else if (n == 0)
        {
            CloseSocket();
            s_state = UPST_COMPARE;
        }
        else
        {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK)
                SetError("Connection lost");
        }
        break;
    }

    // ---- COMPARE: parse body, compare versions ----
    case UPST_COMPARE:
    {
        DWORD cl = 0;
        const char* body = FindBody(s_recvBuf, s_recvLen, &cl);
        if (!body) { SetError("Malformed HTTP response"); break; }

        StrCopy(s_remoteVer, sizeof(s_remoteVer), body);
        StripWhitespace(s_remoteVer);

        if (s_remoteVer[0] == '\0') { SetError("Empty version response"); break; }

        s_isNewer = (VerCmp(s_remoteVer, k_localVersion) > 0);
        s_state = s_isNewer ? UPST_AVAIL : UPST_UP_TO_DATE;
        break;
    }

    // ---- CONNECT_XBE: poll connect, then send GET for .xbe ----
    case UPST_CONNECT_XBE:
    {
        if (!PollConnect()) break;
        s_recvLen = 0;
        ZeroMemory(s_recvBuf, sizeof(s_recvBuf));
        if (!SendGet(k_xbePath)) break;
        s_state = UPST_SEND_XBE;  // transition through SEND_XBE immediately
        // fall through to SEND_XBE on next tick
        break;
    }

    // ---- SEND_XBE: GET sent, move to receive ----
    case UPST_SEND_XBE:
    {
        s_state = UPST_RECV_XBE;
        break;
    }

    // ---- RECV_XBE: stream body into D:\XbDiag.xbe ----
    case UPST_RECV_XBE:
    {
        // Use a stack buffer to recv each tick — avoids a large static download buf.
        // We write directly to file rather than accumulating in RAM.
        char chunk[4096];
        int n = recv(s_sock, chunk, (int)sizeof(chunk), 0);

        if (n > 0)
        {
            // First chunk — check if we still have headers to skip
            if (s_xbeReceived == 0 && s_recvLen == 0)
            {
                // We may need to accumulate header bytes first.
                // Feed into s_recvBuf until we find \r\n\r\n.
                int canAccum = (int)sizeof(s_recvBuf) - s_recvLen - 1;
                int accum = (n < canAccum) ? n : canAccum;
                for (int i = 0; i < accum; ++i)
                    s_recvBuf[s_recvLen + i] = chunk[i];
                s_recvLen += accum;
                s_recvBuf[s_recvLen] = '\0';

                DWORD cl = 0;
                const char* body = FindBody(s_recvBuf, s_recvLen, &cl);
                if (body)
                {
                    s_xbeTotal = cl;
                    // Write body bytes that arrived in this chunk
                    int bodyLen = (int)(s_recvBuf + s_recvLen - body);
                    if (bodyLen > 0)
                    {
                        DWORD written = 0;
                        if (!WriteFile(s_hFile, body, (DWORD)bodyLen, &written, NULL)
                            || written != (DWORD)bodyLen)
                        {
                            SetError("File write error");
                            break;
                        }
                        s_xbeReceived += written;
                    }
                    // Write any remaining bytes from chunk past what we accumulated
                    int remaining = n - accum;
                    if (remaining > 0)
                    {
                        DWORD written = 0;
                        if (!WriteFile(s_hFile, chunk + accum, (DWORD)remaining, &written, NULL)
                            || written != (DWORD)remaining)
                        {
                            SetError("File write error");
                            break;
                        }
                        s_xbeReceived += written;
                    }
                    // Mark header done — subsequent chunks go straight to file
                    // by setting recvLen to a sentinel value > 0
                    s_recvLen = (int)sizeof(s_recvBuf);  // fill marker
                }
                // If header not yet complete, keep accumulating next tick
            }
            else
            {
                // Header already parsed — write chunk directly
                DWORD written = 0;
                if (!WriteFile(s_hFile, chunk, (DWORD)n, &written, NULL)
                    || written != (DWORD)n)
                {
                    SetError("File write error");
                    break;
                }
                s_xbeReceived += written;
            }
        }
        else if (n == 0)
        {
            // Server closed connection — download complete
            CloseSocket();
            CloseFile();
            s_state = UPST_WRITE_DONE;
        }
        else
        {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK)
            {
                SetError("Connection lost during download");
            }
        }
        break;
    }

    // Terminal states — nothing to advance
    case UPST_UP_TO_DATE:
    case UPST_AVAIL:
    case UPST_WRITE_DONE:
    case UPST_ERROR:
    case UPST_IDLE:
    default:
        break;
    }

    Render(logo);
}