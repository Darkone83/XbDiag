// FileExplorerMU.cpp
// XbDiag - Memory Unit mounting, formatting, Skeleton Key, and ENDGAME creation.
//
// MountAllDrives maps HDD partitions to letters C/E/F/G/X/Y/Z.
// MountMUs opens each present MU via MU_CreateDeviceObject and maps A-H.
// FormatMU dismounts, formats as FATX, and remaps the drive letter.
//
// Skeleton Key flow:
//   FE_MU_SKXbaPresent()  — check D:\resources\SK.xba exists
//   FE_MU_DownloadSK()    — DNS + TCP + HTTP GET /xbdiag/resource/SK.xba -> disk
//   FE_MU_CreateSK()      — format MU, Xba_Extract to tmp, copy to MU, cleanup
//
// ENDGAME flow (identical, different asset):
//   FE_MU_EGXbaPresent()  — check D:\resources\ENDGAME.xba exists
//   FE_MU_DownloadEG()    — DNS + TCP + HTTP GET /xbdiag/resource/ENDGAME.xba -> disk
//   FE_MU_CreateEG()      — format MU, Xba_Extract to tmp, copy to MU, cleanup
//
// Both SK and EG delegate to the shared internal helpers:
//   MU_DownloadXba(serverPath, localDest, errOut, errLen)
//   MU_CreateFromXba(localXba, port, slot, errOut, errLen)

#include "FileExplorerMU.h"
#include "FileExplorer.h"
#include "xba.h"
#include <xtl.h>
#include <winsockx.h>
#include "input.h"

// ============================================================================
// Kernel exports
// ============================================================================

extern "C"
{
    LONG  WINAPI MU_CreateDeviceObject(DWORD port, DWORD slot, XBOX_STRING* deviceName);
    VOID  WINAPI MU_CloseDeviceObject(DWORD port, DWORD slot);
    LONG  WINAPI IoDismountVolume(void* deviceObject);
    LONG  WINAPI IoDeleteSymbolicLink(XBOX_STRING* linkName);
    VOID* WINAPI MU_GetExistingDeviceObject(DWORD port, DWORD slot);
    BOOL  WINAPI XapiFormatFATVolumeEx(XBOX_STRING* devicePath, DWORD clusterSize);
}

// ============================================================================
// Drive map
// ============================================================================

struct DriveMap { const char* letter; const char* device; };
static const DriveMap k_drives[] =
{
    { "C", "\\Device\\Harddisk0\\Partition2" },
    { "E", "\\Device\\Harddisk0\\Partition1" },
    { "F", "\\Device\\Harddisk0\\Partition6" },
    { "G", "\\Device\\Harddisk0\\Partition7" },
    { "X", "\\Device\\Harddisk0\\Partition3" },
    { "Y", "\\Device\\Harddisk0\\Partition4" },
    { "Z", "\\Device\\Harddisk0\\Partition5" },
};

// MU drive letter table — indexed by (port*2 + slot), 0-7.
// Avoids HDD letters (C,E,F,G,X,Y,Z) and DVD (D).
//   mu 0 = port0 slot0 = A   mu 1 = port0 slot1 = B
//   mu 2 = port1 slot0 = I   mu 3 = port1 slot1 = J
//   mu 4 = port2 slot0 = K   mu 5 = port2 slot1 = L
//   mu 6 = port3 slot0 = M   mu 7 = port3 slot1 = H
static const char k_muLetters[8] = { 'A', 'B', 'I', 'J', 'K', 'L', 'M', 'H' };

char FE_MU_Letter(int port, int slot)
{
    int mu = port * 2 + slot;
    if (mu < 0 || mu > 7) return '?';
    return k_muLetters[mu];
}

static void MountHDDDrives()
{
    char linkBuf[16];
    for (int i = 0; i < 7; ++i)
    {
        linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
        linkBuf[4] = k_drives[i].letter[0];
        linkBuf[5] = ':'; linkBuf[6] = '\0';
        const char* dev = k_drives[i].device;
        int devLen = 0; while (dev[devLen]) devLen++;
        XBOX_STRING sLink = { 6, 7, linkBuf };
        XBOX_STRING sDev = { (USHORT)devLen, (USHORT)(devLen + 1), (char*)dev };
        IoCreateSymbolicLink(&sLink, &sDev);
    }
}

static bool MountMUs()
{
    bool anyMounted = false;
    for (int port = 0; port < 4; ++port)
    {
        for (int slot = 0; slot < 2; ++slot)
        {
            if (!IsMUPresent(port, slot)) continue;

            char driveLetter = FE_MU_Letter(port, slot);
            char devBuf[64];
            XBOX_STRING devName;
            devName.Length = 0;
            devName.MaximumLength = sizeof(devBuf) - 2;
            devName.Buffer = devBuf;

            if (MU_CreateDeviceObject((DWORD)port, (DWORD)slot, &devName) < 0)
                continue;

            char linkBuf[8];
            linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
            linkBuf[4] = driveLetter; linkBuf[5] = ':'; linkBuf[6] = '\0';
            XBOX_STRING sLink = { 6, 7, linkBuf };
            IoCreateSymbolicLink(&sLink, &devName);
            anyMounted = true;
        }
    }
    return anyMounted;
}

// ============================================================================
// Public API
// ============================================================================

bool FE_MU_MountAll()
{
    MountHDDDrives();
    return MountMUs();
}

// ============================================================================
// MU_Remount — flush and release the MU volume after a write session.
// Dismounts the volume (flushing all pending I/O) and closes the device object
// so the FATX driver fully releases its state.  The next operation will call
// MU_CreateDeviceObject itself to get a clean handle.
// ============================================================================

static void MU_Remount(int port, int slot)
{
    void* devObj = MU_GetExistingDeviceObject((DWORD)port, (DWORD)slot);
    if (devObj) IoDismountVolume(devObj);
    MU_CloseDeviceObject((DWORD)port, (DWORD)slot);
}

bool FE_MU_Format(int port, int slot)
{
    char driveLetter = FE_MU_Letter(port, slot);
    char linkBuf[8];
    char devBuf[64];
    XBOX_STRING devName;
    XBOX_STRING sLink;

    linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
    linkBuf[4] = driveLetter; linkBuf[5] = ':'; linkBuf[6] = '\0';
    sLink = { 6, 7, linkBuf };

    // Step 1: dismount existing volume so all handles are invalidated
    void* devObj = MU_GetExistingDeviceObject((DWORD)port, (DWORD)slot);
    if (devObj) IoDismountVolume(devObj);
    MU_CloseDeviceObject((DWORD)port, (DWORD)slot);

    // Step 2: create device object and format
    devName.Length = 0;
    devName.MaximumLength = sizeof(devBuf) - 2;
    devName.Buffer = devBuf;
    if (MU_CreateDeviceObject((DWORD)port, (DWORD)slot, &devName) < 0)
        return false;

    BOOL ok = XapiFormatFATVolumeEx(&devName, 0);
    if (!ok)
    {
        MU_CloseDeviceObject((DWORD)port, (DWORD)slot);
        return false;
    }

    // Step 3: close and reopen to force FAT driver remount on fresh volume
    MU_CloseDeviceObject((DWORD)port, (DWORD)slot);

    devName.Length = 0;
    devName.MaximumLength = sizeof(devBuf) - 2;
    devName.Buffer = devBuf;
    if (MU_CreateDeviceObject((DWORD)port, (DWORD)slot, &devName) < 0)
        return false;

    // Step 4: rebind drive letter symlink
    IoCreateSymbolicLink(&sLink, &devName);
    return true;
}

// ============================================================================
// Server constants
// Mirror Update.cpp: darkone83.myddns.me:8008
// ============================================================================

static const char* k_muHost = "darkone83.myddns.me";
static const int   k_muPort = 8008;

// Per-asset server paths and local destinations
static const char* k_skPath = "/xbdiag/resource/SK.xba";
static const char* k_skXbaDest = "D:\\resources\\SK.xba";

static const char* k_egPath = "/xbdiag/resource/ENDGAME.xba";
static const char* k_egXbaDest = "D:\\resources\\ENDGAME.xba";

// Shared working directories
static const char* k_muTmpDir = "D:\\resources\\tmp\\";
static const char* k_muResDir = "D:\\resources\\";

// Progress state — written by download/extract/copy, read by FileExplorer render
DWORD g_skProgressDone = 0;   // bytes (download) — not used during SK_CREATING
DWORD g_skProgressTotal = 0;  // bytes (download) — not used during SK_CREATING
int   g_skFilesDone = 0;      // files completed (extract + copy phases)
int   g_skFilesTotal = 0;     // total file count (set by pre-scan, copy adds to it)

// Render pump — FileExplorer sets this before calling blocking operations
FE_MU_RenderFn g_skRenderFn = NULL;

static void XbaProgressCB(int filesDone, int filesTotal,
    DWORD bytesDone, DWORD bytesTotal)
{
    g_skFilesDone = filesDone;
    if (filesTotal > 0) g_skFilesTotal = filesTotal;
    g_skProgressDone = bytesDone;
    // Only pump on file-boundary callbacks, not mid-block byte updates.
    // Mid-write pumps re-enter D3D Present while hOut is open, causing
    // the 0x1E / 0xC0000005 bugcheck on real hardware.
    if (filesDone > 0 && g_skRenderFn) g_skRenderFn();
    (void)bytesTotal;
}

// ============================================================================
// String helpers (no CRT)
// ============================================================================

static int MU_StrLen(const char* s) { int n = 0; while (s[n]) ++n; return n; }

static void MU_StrCopy(char* dst, int dstLen, const char* src)
{
    int i = 0;
    while (src[i] && i < dstLen - 1) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static void MU_AppendStr(char* dst, int dstLen, const char* src)
{
    int i = MU_StrLen(dst);
    while (*src && i < dstLen - 1) dst[i++] = *src++;
    dst[i] = '\0';
}

// ============================================================================
// Recursive directory delete (cleanup tmp)
// ============================================================================

static void MU_DeleteTree(const char* path)
{
    char pattern[256];
    MU_StrCopy(pattern, sizeof(pattern), path);
    int pl = MU_StrLen(pattern);
    if (pl > 0 && pattern[pl - 1] != '\\') { pattern[pl++] = '\\'; pattern[pl] = '\0'; }
    pattern[pl] = '*'; pattern[pl + 1] = '\0';

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.cFileName[0] == '.' &&
                (fd.cFileName[1] == '\0' || (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
                continue;
            char child[256];
            MU_StrCopy(child, sizeof(child), path);
            int cl = MU_StrLen(child);
            if (cl > 0 && child[cl - 1] != '\\') { child[cl++] = '\\'; child[cl] = '\0'; }
            MU_AppendStr(child, sizeof(child), fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                MU_DeleteTree(child);
            else
                DeleteFileA(child);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
}

// ============================================================================
// Count files in a directory tree (for progress pre-scan)
// ============================================================================

static int MU_CountFiles(const char* dir)
{
    char pattern[256];
    WIN32_FIND_DATA fd;
    HANDLE h;
    int total = 0;
    int pl;

    MU_StrCopy(pattern, sizeof(pattern), dir);
    pl = MU_StrLen(pattern);
    if (pl > 0 && pattern[pl - 1] != '\\') { pattern[pl++] = '\\'; pattern[pl] = '\0'; }
    pattern[pl] = '*'; pattern[pl + 1] = '\0';

    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do
    {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' || (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            char child[256];
            MU_StrCopy(child, sizeof(child), dir);
            int cl = MU_StrLen(child);
            if (cl > 0 && child[cl - 1] != '\\') { child[cl++] = '\\'; child[cl] = '\0'; }
            MU_AppendStr(child, sizeof(child), fd.cFileName);
            total += MU_CountFiles(child);
        }
        else
        {
            ++total;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return total;
}

// ============================================================================
// Copy all files from srcDir to dstDir (recursive)
// Used to copy tmp contents to the MU root.
// ============================================================================

static bool MU_CopyDir(const char* srcDir, const char* dstDir,
    char* errOut, int errLen)
{
    char pattern[256];
    MU_StrCopy(pattern, sizeof(pattern), srcDir);
    int pl = MU_StrLen(pattern);
    if (pl > 0 && pattern[pl - 1] != '\\') { pattern[pl++] = '\\'; pattern[pl] = '\0'; }
    pattern[pl] = '*'; pattern[pl + 1] = '\0';

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return true;  // empty src is OK

    static BYTE copyBuf[32768];

    do
    {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' || (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;

        char srcPath[256], dstPath[256];
        MU_StrCopy(srcPath, sizeof(srcPath), srcDir);
        int sl = MU_StrLen(srcPath);
        if (sl > 0 && srcPath[sl - 1] != '\\') { srcPath[sl++] = '\\'; srcPath[sl] = '\0'; }
        MU_AppendStr(srcPath, sizeof(srcPath), fd.cFileName);

        MU_StrCopy(dstPath, sizeof(dstPath), dstDir);
        int dl = MU_StrLen(dstPath);
        if (dl > 0 && dstPath[dl - 1] != '\\') { dstPath[dl++] = '\\'; dstPath[dl] = '\0'; }
        MU_AppendStr(dstPath, sizeof(dstPath), fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (!CreateDirectoryA(dstPath, NULL))
            {
                DWORD err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS)
                {
                    if (errOut)
                    {
                        MU_StrCopy(errOut, errLen, "mkdir fail e=");
                        char eb[12]; IntToStr((int)err, eb, sizeof(eb));
                        MU_AppendStr(errOut, errLen, eb);
                        MU_AppendStr(errOut, errLen, " ");
                        MU_AppendStr(errOut, errLen, dstPath);
                    }
                    FindClose(h); return false;
                }
            }
            if (!MU_CopyDir(srcPath, dstPath, errOut, errLen))
            {
                FindClose(h); return false;
            }
        }
        else
        {
            HANDLE hSrc = CreateFileA(srcPath, GENERIC_READ, FILE_SHARE_READ,
                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hSrc == INVALID_HANDLE_VALUE)
            {
                DWORD err = GetLastError();
                if (errOut)
                {
                    MU_StrCopy(errOut, errLen, "open src fail e=");
                    char eb[12]; IntToStr((int)err, eb, sizeof(eb));
                    MU_AppendStr(errOut, errLen, eb);
                }
                FindClose(h); return false;
            }

            HANDLE hDst = CreateFileA(dstPath, GENERIC_WRITE, 0,
                NULL, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
            if (hDst == INVALID_HANDLE_VALUE)
            {
                DWORD err = GetLastError();
                if (errOut)
                {
                    MU_StrCopy(errOut, errLen, "create dst fail e=");
                    char eb[12]; IntToStr((int)err, eb, sizeof(eb));
                    MU_AppendStr(errOut, errLen, eb);
                    MU_AppendStr(errOut, errLen, " ");
                    MU_AppendStr(errOut, errLen, dstPath);
                }
                CloseHandle(hSrc); FindClose(h); return false;
            }

            bool ok = true;
            DWORD nr = 0;
            DWORD pump2 = 0;
            while (ReadFile(hSrc, copyBuf, sizeof(copyBuf), &nr, NULL) && nr > 0)
            {
                DWORD nw = 0;
                if (!WriteFile(hDst, copyBuf, nr, &nw, NULL) || nw != nr)
                {
                    DWORD err = GetLastError();
                    if (errOut)
                    {
                        MU_StrCopy(errOut, errLen, "write fail e=");
                        char eb[12]; IntToStr((int)err, eb, sizeof(eb));
                        MU_AppendStr(errOut, errLen, eb);
                        MU_AppendStr(errOut, errLen, " ");
                        MU_AppendStr(errOut, errLen, fd.cFileName);
                    }
                    ok = false; break;
                }
                g_skProgressDone += nw;
                pump2 += nw;
                if (pump2 >= 65536 && g_skRenderFn)
                {
                    g_skRenderFn();
                    pump2 = 0;
                }
            }
            CloseHandle(hDst);
            CloseHandle(hSrc);
            if (!ok) { FindClose(h); return false; }

            // Count this file as done and pump render so the progress bar advances.
            ++g_skFilesDone;
            if (g_skRenderFn) g_skRenderFn();
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return true;
}

// ============================================================================
// HTTP helpers (self-contained, no Update.cpp dependencies)
// ============================================================================

static int MU_FindHeaderEnd(const char* buf, int len)
{
    for (int i = 0; i < len - 3; ++i)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return i + 4;
    return -1;
}

static int MU_GetHttpStatus(const char* buf, int len)
{
    if (len < 12 || buf[0] != 'H' || buf[1] != 'T' || buf[2] != 'T' || buf[3] != 'P') return 0;
    int i = 0;
    while (i < len && buf[i] != ' ') ++i;
    while (i < len && buf[i] == ' ') ++i;
    if (i + 3 > len) return 0;
    int code = 0;
    for (int j = 0; j < 3 && buf[i + j] >= '0' && buf[i + j] <= '9'; ++j)
        code = code * 10 + (int)(buf[i + j] - '0');
    return code;
}

static DWORD MU_ParseContentLength(const char* buf, int bodyStart)
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

// ============================================================================
// MU_DownloadXba — shared download core
// Downloads the .xba at serverPath to localDest.
// Mirrors Update.cpp DoDownload exactly:
//   1. XNetStartup / WSAStartup (safe to call repeatedly)
//   2. XNetGetTitleXnAddr link check (3s poll)
//   3. XNetDnsLookup + poll (5s timeout)
//   4. Non-blocking connect + select() 5s timeout
//   5. Switch to blocking SO_RCVTIMEO/SO_SNDTIMEO 10s
//   6. HTTP/1.0 GET, accumulate header until \r\n\r\n, verify HTTP 200
//      — status code included in error message for diagnostics
//   7. Write body to file (overflow bytes from header buffer first),
//      FlushFileBuffers before CloseHandle
// ============================================================================

static bool MU_DownloadXba(const char* serverPath, const char* localDest,
    char* errMsgOut, int errMsgLen)
{
    // Reset progress before download starts
    g_skProgressDone = 0;
    g_skProgressTotal = 0;
    g_skFilesDone = 0;

    // Ensure D:\resources\ exists
    CreateDirectoryA(k_muResDir, NULL);

    // Network stack — safe to call repeatedly (ref-counted on Xbox)
    {
        XNetStartupParams xnsp; ZeroMemory(&xnsp, sizeof(xnsp));
        xnsp.cfgSizeOfStruct = sizeof(xnsp);
        xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
        XNetStartup(&xnsp);
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    }

    // Link check — mirrors BootCheckPoll UPST_NET_INIT (3s timeout)
    {
        XNADDR xna; ZeroMemory(&xna, sizeof(xna));
        DWORD linkStart = GetTickCount();
        DWORD st;
        do {
            st = XNetGetTitleXnAddr(&xna);
            if (st != XNET_GET_XNADDR_PENDING) break;
            Sleep(100);
        } while (GetTickCount() - linkStart < 3000);

        if ((st & XNET_GET_XNADDR_NONE) || xna.ina.s_addr == 0)
        {
            MU_StrCopy(errMsgOut, errMsgLen, "No network link");
            return false;
        }
    }

    // DNS lookup
    XNDNS* dns = NULL;
    if (XNetDnsLookup(k_muHost, NULL, &dns) != 0 || !dns)
    {
        MU_StrCopy(errMsgOut, errMsgLen, "DNS lookup failed");
        return false;
    }
    {
        DWORD dnsStart = GetTickCount();
        while (dns->iStatus == WSAEINPROGRESS)
        {
            if (GetTickCount() - dnsStart > 5000)
            {
                XNetDnsRelease(dns);
                MU_StrCopy(errMsgOut, errMsgLen, "DNS timeout");
                return false;
            }
            Sleep(50);
        }
        if (dns->iStatus != 0)
        {
            XNetDnsRelease(dns);
            MU_StrCopy(errMsgOut, errMsgLen, "DNS failed");
            return false;
        }
    }
    IN_ADDR serverAddr = dns->aina[0];
    XNetDnsRelease(dns); dns = NULL;

    // TCP connect — non-blocking with 5s select() timeout
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
    {
        MU_StrCopy(errMsgOut, errMsgLen, "socket() failed");
        return false;
    }
    u_long nb = 1; ioctlsocket(sock, FIONBIO, &nb);
    sockaddr_in sa; ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)k_muPort);
    sa.sin_addr = serverAddr;
    int cr = connect(sock, (sockaddr*)&sa, sizeof(sa));
    if (cr == SOCKET_ERROR)
    {
        if (WSAGetLastError() != WSAEWOULDBLOCK)
        {
            closesocket(sock);
            MU_StrCopy(errMsgOut, errMsgLen, "connect() failed");
            return false;
        }
        fd_set wset; FD_ZERO(&wset); FD_SET(sock, &wset);
        TIMEVAL tv; tv.tv_sec = 5; tv.tv_usec = 0;
        if (select(0, NULL, &wset, NULL, &tv) <= 0)
        {
            closesocket(sock);
            MU_StrCopy(errMsgOut, errMsgLen, "Connect timeout");
            return false;
        }
    }

    // Switch to blocking with 10s timeouts
    nb = 0; ioctlsocket(sock, FIONBIO, &nb);
    int tmo = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));

    // HTTP GET
    char req[256];
    MU_StrCopy(req, sizeof(req), "GET ");
    MU_AppendStr(req, sizeof(req), serverPath);
    MU_AppendStr(req, sizeof(req), " HTTP/1.0\r\nHost: ");
    MU_AppendStr(req, sizeof(req), k_muHost);
    MU_AppendStr(req, sizeof(req), "\r\nConnection: close\r\n\r\n");
    if (send(sock, req, MU_StrLen(req), 0) <= 0)
    {
        closesocket(sock);
        MU_StrCopy(errMsgOut, errMsgLen, "Send failed");
        return false;
    }

    // Accumulate headers until \r\n\r\n
    static char hdrBuf[2048];
    int hdrLen = 0;
    while (hdrLen < (int)sizeof(hdrBuf) - 1)
    {
        int n = recv(sock, hdrBuf + hdrLen, (int)sizeof(hdrBuf) - 1 - hdrLen, 0);
        if (n <= 0) break;
        hdrLen += n; hdrBuf[hdrLen] = '\0';
        if (MU_FindHeaderEnd(hdrBuf, hdrLen) >= 0) break;
    }

    int bodyStart = MU_FindHeaderEnd(hdrBuf, hdrLen);
    if (bodyStart < 0)
    {
        closesocket(sock);
        MU_StrCopy(errMsgOut, errMsgLen, "No HTTP header");
        return false;
    }

    int httpCode = MU_GetHttpStatus(hdrBuf, hdrLen);
    if (httpCode != 200)
    {
        char codeStr[8];
        codeStr[0] = '0' + (char)(httpCode / 100);
        codeStr[1] = '0' + (char)((httpCode / 10) % 10);
        codeStr[2] = '0' + (char)(httpCode % 10);
        codeStr[3] = '\0';
        closesocket(sock);
        MU_StrCopy(errMsgOut, errMsgLen, "HTTP ");
        MU_AppendStr(errMsgOut, errMsgLen, httpCode > 0 ? codeStr : "error");
        return false;
    }

    DWORD cl = MU_ParseContentLength(hdrBuf, bodyStart);
    g_skProgressTotal = cl;
    g_skProgressDone = 0;

    // Open output file
    HANDLE hf = CreateFileA(localDest, GENERIC_WRITE, 0,
        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        closesocket(sock);
        MU_StrCopy(errMsgOut, errMsgLen, "Cannot create file");
        return false;
    }

    // Write overflow bytes already in header buffer
    DWORD totalRecv = 0;
    int overflow = hdrLen - bodyStart;
    if (overflow > 0)
    {
        DWORD wr = 0;
        WriteFile(hf, hdrBuf + bodyStart, (DWORD)overflow, &wr, NULL);
        totalRecv += wr;
    }

    // Stream body
    static BYTE dlBuf[4096];
    DWORD pumpAccum = 0;
    while (true)
    {
        int n = recv(sock, (char*)dlBuf, sizeof(dlBuf), 0);
        if (n <= 0) break;
        DWORD wr = 0;
        WriteFile(hf, dlBuf, (DWORD)n, &wr, NULL);
        totalRecv += wr;
        g_skProgressDone = totalRecv;
        pumpAccum += wr;
        if (pumpAccum >= 65536 && g_skRenderFn)
        {
            g_skRenderFn();
            pumpAccum = 0;
        }
        if (cl > 0 && totalRecv >= cl) break;
    }

    FlushFileBuffers(hf);
    CloseHandle(hf);
    closesocket(sock);

    if (totalRecv == 0)
    {
        DeleteFileA(localDest);
        MU_StrCopy(errMsgOut, errMsgLen, "Empty response");
        return false;
    }

    return true;
}

// ============================================================================
// MU_CreateFromXba — shared create core
// Extract xbaDest to tmp, format MU, copy tmp to MU root, cleanup.
// ============================================================================

static bool MU_CreateFromXba(const char* xbaDest, int port, int slot,
    char* errMsgOut, int errMsgLen)
{
    // Always clean up tmp on entry
    MU_DeleteTree(k_muTmpDir);

    // Ensure resources dir exists
    CreateDirectoryA(k_muResDir, NULL);

    // Phase 1: extract .xba to tmp
    CreateDirectoryA(k_muTmpDir, NULL);
    g_skProgressDone = 0;
    g_skProgressTotal = 0;
    g_skFilesDone = 0;
    g_skFilesTotal = 0;
    char xbaDetail[128] = {};
    XbaResult xr = Xba_Extract(xbaDest, k_muTmpDir, XbaProgressCB,
        xbaDetail, sizeof(xbaDetail));
    if (xr != XBA_OK)
    {
        MU_DeleteTree(k_muTmpDir);
        MU_StrCopy(errMsgOut, errMsgLen, Xba_ResultStr(xr));
        if (xbaDetail[0])
        {
            MU_AppendStr(errMsgOut, errMsgLen, ": ");
            MU_AppendStr(errMsgOut, errMsgLen, xbaDetail);
        }
        return false;
    }

    // Phase 2: format MU
    if (!FE_MU_Format(port, slot))
    {
        MU_DeleteTree(k_muTmpDir);
        MU_StrCopy(errMsgOut, errMsgLen, "Format failed");
        return false;
    }

    // Phase 3: copy tmp contents to MU root
    char muRoot[8];
    muRoot[0] = FE_MU_Letter(port, slot);
    muRoot[1] = ':'; muRoot[2] = '\\'; muRoot[3] = '\0';

    // Pre-scan copy source so the progress bar knows the combined total.
    // g_skFilesTotal already holds the extract file count from XbaProgressCB;
    // add the files-to-copy so the bar covers both phases without resetting.
    g_skFilesTotal += MU_CountFiles(k_muTmpDir);
    g_skFilesDone = 0;

    if (!MU_CopyDir(k_muTmpDir, muRoot, errMsgOut, errMsgLen))
    {
        MU_DeleteTree(k_muTmpDir);
        return false;
    }

    // Phase 4: cleanup tmp
    MU_DeleteTree(k_muTmpDir);

    // Phase 5: remount — dismount/close/reopen the volume so the FATX driver
    // releases the write session.  Without this the next operation on the same
    // MU returns ERROR_DISK_FULL (112) because the stale volume object thinks
    // it still owns all the space written during the copy phase.
    MU_Remount(port, slot);
    return true;
}

// ============================================================================
// FE_MU_SKXbaPresent / FE_MU_DownloadSK / FE_MU_CreateSK
// ============================================================================

bool FE_MU_SKXbaPresent()
{
    DWORD attr = GetFileAttributesA(k_skXbaDest);
    return (attr != 0xFFFFFFFF && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

bool FE_MU_DownloadSK(char* errMsgOut, int errMsgLen)
{
    return MU_DownloadXba(k_skPath, k_skXbaDest, errMsgOut, errMsgLen);
}

bool FE_MU_CreateSK(int port, int slot, char* errMsgOut, int errMsgLen)
{
    return MU_CreateFromXba(k_skXbaDest, port, slot, errMsgOut, errMsgLen);
}

// ============================================================================
// FE_MU_EGXbaPresent / FE_MU_DownloadEG / FE_MU_CreateEG
// ============================================================================

bool FE_MU_EGXbaPresent()
{
    DWORD attr = GetFileAttributesA(k_egXbaDest);
    return (attr != 0xFFFFFFFF && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

bool FE_MU_DownloadEG(char* errMsgOut, int errMsgLen)
{
    return MU_DownloadXba(k_egPath, k_egXbaDest, errMsgOut, errMsgLen);
}

bool FE_MU_CreateEG(int port, int slot, char* errMsgOut, int errMsgLen)
{
    return MU_CreateFromXba(k_egXbaDest, port, slot, errMsgOut, errMsgLen);
}