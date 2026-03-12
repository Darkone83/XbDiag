// FileExplorer.cpp
// XbDiag - Single-pane file explorer with minimal FTP server.
//
// ===========================================================================
// Navigation
// ===========================================================================
//
//  [A]           Enter directory / (at drive list) open drive
//  [B]           Go up one level  (at drive list = back to menu)
//  [X]           Launch selected XBE via XLaunchNewImage (one-way, no return)
//  [DPad Up/Dn]  Move cursor
//  [LT / RT]     Page up / page down
//  [Start]       Toggle FTP server on / off
//
// ===========================================================================
// FTP server — bare minimum passive-mode only
// ===========================================================================
//
//  Credentials : xbox / xbox
//  Port        : 21
//  Mode        : passive (PASV) only — no PORT/active support
//  Clients     : one at a time
//  Commands    : USER PASS SYST TYPE PWD CWD CDUP LIST RETR STOR PASV QUIT
//  All sockets : non-blocking (FIONBIO) — polled every tick, never stalls
//
//  State machine (s_ftp.*):
//    FTP_OFF       - server socket not open
//    FTP_LISTEN    - server socket open, waiting for client
//    FTP_CONNECTED - control connection established
//    FTP_TRANSFER  - data connection active (LIST / RETR / STOR)
//
// ===========================================================================
// FTP status widget — lower-right corner above bot bar
// ===========================================================================
//
//  When OFF    : nothing drawn (hint in bot bar only)
//  When ON     :
//    ┌─────────────────────┐
//    │ FTP  ● LISTENING    │
//    │ 192.168.1.42 :21    │
//    └─────────────────────┘
//  During transfer:
//    ┌─────────────────────┐
//    │ FTP  ● CONNECTED    │
//    │ 192.168.1.42 :21    │
//    │ STOR  filename.bin  │  (truncated to 18 chars)
//    │ [████████░░]  64%   │
//    └─────────────────────┘
//
// ===========================================================================
// Layout (640x480 design space)
// ===========================================================================
//
//  TOP BAR     DrawPageChrome                  Y=0..58
//  Path bar    current path                    Y=58..74   H=16
//  ─────────────────────────────────────────────────────────────
//  File list   scrollable, 1 row = 18px        Y=74..436
//  ─────────────────────────────────────────────────────────────
//  BOT BAR     DrawPageChrome hints            Y=450..480
//
// ===========================================================================

#include "FileExplorer.h"
#include "font.h"
#include "input.h"
#include <xtl.h>
#include <winsockx.h>

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

// ============================================================================
// Kernel exports needed for drive mounting
// ============================================================================

typedef struct _XBOX_STRING {
    USHORT Length;
    USHORT MaximumLength;
    char* Buffer;
} XBOX_STRING;

extern "C" LONG WINAPI IoCreateSymbolicLink(XBOX_STRING* symLink, XBOX_STRING* target);
extern "C" LONG WINAPI IoDeleteSymbolicLink(XBOX_STRING* symLink);

// ============================================================================
// Constants
// ============================================================================

#define MAX_ENTRIES     256     // max files/dirs shown in one directory
#define MAX_PATH_LEN    256
#define MAX_NAME_LEN    64
#define ROWS_VISIBLE    20      // file rows that fit between path bar and bot bar
#define ROW_H           (LINE_H + 1.f)
#define LIST_Y          74.f
#define PATH_Y          60.f

// FTP widget geometry (lower-right, above bot bar)
#define WIDGET_W        220.f
#define WIDGET_X        (SW - LM - WIDGET_W)
#define WIDGET_PAD      6.f
#define WIDGET_LINE_H   14.f

// FTP data port for passive connections
#define FTP_CTRL_PORT   21
#define FTP_DATA_PORT_BASE  2024   // passive port range: 2024–2055
#define FTP_DATA_PORT_COUNT   32
static int s_nextDataPort = 0;     // cycles through the range

// File operation limits
#define MAX_CLIPBOARD   64      // max items in clipboard/selection
#define COPY_BUF_SIZE   (64*1024)  // 64 KB copy chunk

// ============================================================================
// Types
// ============================================================================

struct FileEntry
{
    char  name[MAX_NAME_LEN];
    bool  isDir;
    DWORD sizeLow;    // 0 for directories
};

enum FtpState
{
    FTP_OFF = 0,
    FTP_LISTEN,
    FTP_CONNECTED,
    FTP_TRANSFER,
};

enum FtpXfer { XFER_NONE = 0, XFER_LIST, XFER_RETR, XFER_STOR };

struct FtpCtx
{
    FtpState state;

    // Sockets
    SOCKET   listenSock;   // server accept socket
    SOCKET   ctrlSock;     // connected client control socket
    SOCKET   dataListen;   // passive listen socket
    SOCKET   dataSock;     // connected data socket
    WORD     dataPort;     // port dataListen is bound to (set by FtpOpenPassive)

    // Auth
    bool     authed;
    bool     gotUser;
    bool     atVirtualRoot;  // cwd is the synthetic drive-list root "/"

    // Current working directory (tracks explorer path by default)
    char     cwd[MAX_PATH_LEN];

    // Transfer state
    FtpXfer  xferType;
    char     xferName[MAX_NAME_LEN];   // display name (truncated if needed)
    char     xferPath[MAX_PATH_LEN];   // full path
    HANDLE   xferFile;
    DWORD    xferTotal;    // file size (0 = unknown / directory listing)
    DWORD    xferDone;

    // Rename state (RNFR/RNTO)
    bool     gotRnfr;
    char     rnfrPath[MAX_PATH_LEN];

    // Receive buffer for control commands
    char     recvBuf[1024];
    int      recvLen;

    // Send buffer for control replies — drains each tick to handle WSAEWOULDBLOCK
    char     sendBuf[2048];
    int      sendLen;   // bytes queued
    int      sendOff;   // bytes already sent

    // RETR partial-send buffer — holds unsent remainder between ticks
    char     retrBuf[4096];
    int      retrBufLen;   // bytes valid in retrBuf
    int      retrBufOff;   // bytes already sent

    // Deferred LIST — populated when LIST arrives before dataSock is ready
    bool     listPending;
    bool     listVirtualRoot;
    char     listDir[MAX_PATH_LEN];

    // Buffered LIST data — built by FtpDoListing, drained by FtpTick XFER_LIST
    // 64 KB covers ~800 files; more than enough for any real Xbox directory
    char     listBuf[65536];
    int      listBufLen;
    int      listBufOff;
};

// ============================================================================
// Module state
// ============================================================================

// --- Explorer ---
static FileEntry s_entries[MAX_ENTRIES];
static int       s_entryCount = 0;
static int       s_cursor = 0;
static int       s_scroll = 0;
static bool      s_atRoot = true;   // showing synthetic drive list
static char      s_path[MAX_PATH_LEN];   // current directory path (empty at root)
static bool      s_skipFirstTick = true;
static WORD      s_prevBtns = 0;

// --- Network ---
static char      s_ipStr[20];   // dotted decimal or "No Link"
static bool      s_ipOK = false;

// --- FTP status message (shown briefly in path bar on toggle failure) ---
static char      s_ftpMsg[48] = { 0 };
static int       s_ftpMsgFrames = 0;

// --- File operation state ---
enum FileOpType { FILEOP_NONE = 0, FILEOP_COPY, FILEOP_MOVE };
enum FileOpState { FOS_IDLE = 0, FOS_CONFIRM_DELETE, FOS_RUNNING };

struct ClipboardEntry
{
    char path[MAX_PATH_LEN];   // full source path
    bool isDir;
};

static ClipboardEntry s_clipboard[MAX_CLIPBOARD];
static int            s_clipCount = 0;
static FileOpType     s_clipOp = FILEOP_NONE;  // COPY or MOVE

// Selection (marked items — persists across navigation)
static bool           s_marked[MAX_ENTRIES];        // parallel to s_entries
static int            s_markedCount = 0;

// Confirm-delete overlay
static FileOpState    s_fosState = FOS_IDLE;

// Flat work-list for tick-driven file op
// Dirs are pre-expanded into mkdir entries + file entries at op start.
#define MAX_WORK_ITEMS  2048

enum WorkItemType { WI_MKDIR = 0, WI_FILE };

struct WorkItem
{
    WorkItemType type;
    char src[MAX_PATH_LEN];   // source path  (WI_FILE only)
    char dst[MAX_PATH_LEN];   // destination path
};

static WorkItem  s_work[MAX_WORK_ITEMS];
static int       s_workCount = 0;
static int       s_workIdx = 0;       // current item being processed
static FileOpType s_workOp = FILEOP_NONE;
static char      s_workDstRoot[MAX_PATH_LEN] = {};  // destination root for this op

// Per-tick file copy state
static HANDLE    s_opSrcHandle = INVALID_HANDLE_VALUE;
static HANDLE    s_opDstHandle = INVALID_HANDLE_VALUE;
static bool      s_opRunning = false;
static char      s_opSrcName[MAX_NAME_LEN] = {};
static DWORD     s_opDone = 0;
static DWORD     s_opTotal = 0;
static int       s_opItemDone = 0;
static int       s_opItemTotal = 0;

// --- FTP ---
static FtpCtx    s_ftp;


// ============================================================================
// Utility helpers
// ============================================================================

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

static void AppendStr(char* out, int outLen, const char* src)
{
    int i = 0; while (out[i]) i++;
    while (*src && i < outLen - 1) out[i++] = *src++;
    out[i] = '\0';
}

// Format file size — bytes if < 1024, KB if < 1MB, MB otherwise
static void FormatSize(DWORD bytes, char* buf, int bufLen)
{
    if (bytes < 1024)
    {
        IntToStr((int)bytes, buf, bufLen);
        AppendStr(buf, bufLen, " B");
    }
    else if (bytes < 1024 * 1024)
    {
        IntToStr((int)(bytes / 1024), buf, bufLen);
        AppendStr(buf, bufLen, " KB");
    }
    else
    {
        IntToStr((int)(bytes / (1024 * 1024)), buf, bufLen);
        AppendStr(buf, bufLen, " MB");
    }
}

// Truncate name to maxChars, appending ".." if truncated
static void TruncName(const char* src, char* dst, int maxChars, int dstLen)
{
    int len = 0;
    while (src[len]) len++;
    if (len <= maxChars)
    {
        StrCopy(dst, dstLen, src);
        return;
    }
    // Copy maxChars-2 chars then ".."
    int i = 0;
    for (; i < maxChars - 2 && i < dstLen - 3; ++i)
        dst[i] = src[i];
    dst[i++] = '.';
    dst[i++] = '.';
    dst[i] = '\0';
}

// Check if filename ends in .xbe (case-insensitive)
static bool IsXBE(const char* name)
{
    int len = 0;
    while (name[len]) len++;
    if (len < 4) return false;
    const char* ext = name + len - 4;
    return (ext[0] == '.' &&
        (ext[1] == 'x' || ext[1] == 'X') &&
        (ext[2] == 'b' || ext[2] == 'B') &&
        (ext[3] == 'e' || ext[3] == 'E'));
}

// ============================================================================
// Directory loading
// ============================================================================
// Drive mounting — map standard HDD partitions to drive letters so
// FindFirstFile can see them.  Mirrors PrometheOS drive.cpp mount logic.
// IoCreateSymbolicLink returns 0 on success or STATUS_OBJECT_NAME_COLLISION
// (0xC0000035) if already mapped — both are fine; we ignore the return.
// ============================================================================

struct DriveMap { const char* letter; const char* device; };
static const DriveMap k_drives[] =
{
    { "C", "\\Device\\Harddisk0\\Partition2" },  // C: System
    { "E", "\\Device\\Harddisk0\\Partition1" },  // E: Data (large)
    { "F", "\\Device\\Harddisk0\\Partition6" },  // F: Extended (if present)
    { "G", "\\Device\\Harddisk0\\Partition7" },  // G: Extended (if present)
    { "X", "\\Device\\Harddisk0\\Partition3" },  // X: Cache0
    { "Y", "\\Device\\Harddisk0\\Partition4" },  // Y: Cache1
    { "Z", "\\Device\\Harddisk0\\Partition5" },  // Z: Cache2
    // D: is the DVD/utility drive — always mounted by the kernel
};

static void MountAllDrives()
{
    char linkBuf[16];
    for (int i = 0; i < 7; ++i)
    {
        // Build "\\??\\X:" style symbolic link name
        linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
        linkBuf[4] = k_drives[i].letter[0];
        linkBuf[5] = ':'; linkBuf[6] = '\0';

        const char* dev = k_drives[i].device;
        int devLen = 0; while (dev[devLen]) devLen++;
        int lnkLen = 6; // "\\??\\ X:"

        XBOX_STRING sLink = { (USHORT)lnkLen, (USHORT)(lnkLen + 1), linkBuf };
        XBOX_STRING sDev = { (USHORT)devLen, (USHORT)(devLen + 1), (char*)dev };
        IoCreateSymbolicLink(&sLink, &sDev);
        // Ignore return — collision (already mounted) is fine
    }
}

// ============================================================================
static void LoadDriveList()
{
    s_entryCount = 0;
    s_atRoot = true;
    s_path[0] = '\0';
    for (int i = 0; i < MAX_ENTRIES; ++i) s_marked[i] = false;
    s_markedCount = 0;

    // Ensure HDD partitions are mapped to drive letters before probing
    MountAllDrives();
    const char* drives[] = { "C", "D", "E", "F", "G", "X", "Y", "Z" };
    for (int d = 0; d < 8 && s_entryCount < MAX_ENTRIES; ++d)
    {
        char pattern[8];
        pattern[0] = drives[d][0]; pattern[1] = ':'; pattern[2] = '\\';
        pattern[3] = '*'; pattern[4] = '\0';

        WIN32_FIND_DATA fd;
        HANDLE h = FindFirstFile(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            FindClose(h);
            FileEntry& e = s_entries[s_entryCount++];
            e.name[0] = drives[d][0]; e.name[1] = ':'; e.name[2] = '\0';
            e.isDir = true;
            e.sizeLow = 0;
        }
    }
}

static void LoadDirectory(const char* path)
{
    s_entryCount = 0;
    s_atRoot = false;
    StrCopy(s_path, sizeof(s_path), path);
    for (int i = 0; i < MAX_ENTRIES; ++i) s_marked[i] = false;
    s_markedCount = 0;

    char pattern[MAX_PATH_LEN + 4];
    StrCopy(pattern, sizeof(pattern), path);
    // Ensure trailing backslash
    int plen = 0; while (pattern[plen]) plen++;
    if (plen > 0 && pattern[plen - 1] != '\\')
    {
        pattern[plen] = '\\'; pattern[plen + 1] = '\0'; plen++;
    }
    pattern[plen] = '*'; pattern[plen + 1] = '\0';

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do
    {
        // Skip . and ..
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' || (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;

        if (s_entryCount >= MAX_ENTRIES) break;

        FileEntry& e = s_entries[s_entryCount++];
        TruncName(fd.cFileName, e.name, MAX_NAME_LEN - 1, MAX_NAME_LEN);
        e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.sizeLow = e.isDir ? 0 : fd.nFileSizeLow;
    } while (FindNextFile(h, &fd));

    FindClose(h);

    // Sort: directories first, then files, both alphabetically (simple bubble)
    for (int i = 0; i < s_entryCount - 1; ++i)
        for (int j = i + 1; j < s_entryCount; ++j)
        {
            bool swap = false;
            if (s_entries[i].isDir == s_entries[j].isDir)
            {
                // Same type: alpha compare
                const char* a = s_entries[i].name;
                const char* b = s_entries[j].name;
                int k = 0;
                while (a[k] && b[k] && a[k] == b[k]) k++;
                if (a[k] > b[k]) swap = true;
            }
            else if (!s_entries[i].isDir && s_entries[j].isDir)
                swap = true;

            if (swap)
            {
                FileEntry tmp = s_entries[i];
                s_entries[i] = s_entries[j];
                s_entries[j] = tmp;
            }
        }
}

// Navigate into entry at cursor
static void EnterSelected()
{
    if (s_entryCount == 0) return;
    FileEntry& e = s_entries[s_cursor];

    if (s_atRoot)
    {
        // e.name is "C:" etc — open as "C:\"
        char drivePath[8];
        drivePath[0] = e.name[0]; drivePath[1] = ':';
        drivePath[2] = '\\'; drivePath[3] = '\0';
        LoadDirectory(drivePath);
    }
    else if (e.isDir)
    {
        char newPath[MAX_PATH_LEN];
        StrCopy(newPath, sizeof(newPath), s_path);
        int plen = 0; while (newPath[plen]) plen++;
        if (plen > 0 && newPath[plen - 1] != '\\')
        {
            newPath[plen] = '\\'; newPath[plen + 1] = '\0'; plen++;
        }
        StrCat2(newPath, sizeof(newPath), newPath, e.name);
        LoadDirectory(newPath);
    }

    s_cursor = 0;
    s_scroll = 0;
}

// Navigate up one level
static void GoUp()
{
    if (s_atRoot)
    {
        RequestState(MSTATE_MENU);
        return;
    }

    // Find last backslash — if it's right after the drive colon, go to root
    int len = 0;
    while (s_path[len]) len++;

    // Strip trailing backslash if present
    int end = len;
    if (end > 0 && s_path[end - 1] == '\\') end--;

    // Find previous backslash
    int slash = -1;
    for (int i = end - 1; i >= 0; --i)
    {
        if (s_path[i] == '\\') { slash = i; break; }
    }

    if (slash <= 2)  // "C:\" or no slash = we're at drive root
    {
        LoadDriveList();
    }
    else
    {
        char parent[MAX_PATH_LEN];
        int i = 0;
        for (; i < slash; ++i) parent[i] = s_path[i];
        parent[i] = '\0';
        LoadDirectory(parent);
    }

    s_cursor = 0;
    s_scroll = 0;
}

// ============================================================================
// IP resolution (same pattern as SysInfo)
// ============================================================================

static void ResolveIP()
{
    StrCopy(s_ipStr, sizeof(s_ipStr), "No Link");
    s_ipOK = false;

    XNADDR xna;
    ZeroMemory(&xna, sizeof(xna));
    DWORD st = XNetGetTitleXnAddr(&xna);
    // Short poll — OnEnter already called XNetStartup so DHCP had time
    for (int i = 0; i < 20 && st == XNET_GET_XNADDR_PENDING; ++i)
    {
        Sleep(50);
        st = XNetGetTitleXnAddr(&xna);
    }
    if (!(st & XNET_GET_XNADDR_NONE) && xna.ina.s_addr != 0)
    {
        BYTE* b = (BYTE*)&xna.ina.s_addr;
        char* p = s_ipStr;
        char oct[6];
        for (int oi = 0; oi < 4; ++oi)
        {
            IntToStr((int)b[oi], oct, sizeof(oct));
            const char* sp = oct;
            while (*sp) *p++ = *sp++;
            if (oi < 3) *p++ = '.';
        }
        *p = '\0';
        s_ipOK = true;
    }
}

// ============================================================================
// FTP helpers
// ============================================================================

static void FtpSendStr(SOCKET s, const char* str)
{
    // Queue into ctrl send buffer; FtpTick drains it each frame.
    if (s != s_ftp.ctrlSock) return;  // only used for ctrl replies
    int len = 0; while (str[len]) len++;

    // If the reply won't fit, first compact the buffer by shifting out the
    // already-sent prefix. sendOff bytes at the front are gone — reclaim them.
    int space = (int)sizeof(s_ftp.sendBuf) - s_ftp.sendLen;
    if (len > space && s_ftp.sendOff > 0)
    {
        int remaining = s_ftp.sendLen - s_ftp.sendOff;
        for (int i = 0; i < remaining; ++i)
            s_ftp.sendBuf[i] = s_ftp.sendBuf[s_ftp.sendOff + i];
        s_ftp.sendLen = remaining;
        s_ftp.sendOff = 0;
        space = (int)sizeof(s_ftp.sendBuf) - s_ftp.sendLen;
    }

    // After compaction, if it still won't fit the buffer is genuinely exhausted.
    // This should be unreachable in normal operation (2048 bytes, command intake
    // paused at threshold) — but if it happens, stay connected and discard rather
    // than drop the session. A missed reply is recoverable; a disconnect is not.
    if (len > space) return;

    for (int i = 0; i < len; ++i)
        s_ftp.sendBuf[s_ftp.sendLen++] = str[i];
}

static void FtpReply(int code, const char* msg)
{
    char line[128];
    char codeBuf[8];
    IntToStr(code, codeBuf, sizeof(codeBuf));
    line[0] = '\0';
    StrCat2(line, sizeof(line), line, codeBuf);
    StrCat2(line, sizeof(line), line, " ");
    StrCat2(line, sizeof(line), line, msg);
    StrCat2(line, sizeof(line), line, "\r\n");
    FtpSendStr(s_ftp.ctrlSock, line);
}

// Open a passive data listen socket and return the port
static bool FtpOpenPassive()
{
    if (s_ftp.dataListen != INVALID_SOCKET)
    {
        closesocket(s_ftp.dataListen);
        s_ftp.dataListen = INVALID_SOCKET;
    }
    if (s_ftp.dataSock != INVALID_SOCKET)
    {
        closesocket(s_ftp.dataSock);
        s_ftp.dataSock = INVALID_SOCKET;
    }

    s_ftp.dataListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_ftp.dataListen == INVALID_SOCKET) return false;

    // SO_REUSEADDR so rebind succeeds immediately after prior connection closes
    DWORD reuse = 1;
    setsockopt(s_ftp.dataListen, SOL_SOCKET, SO_REUSEADDR,
        (const char*)&reuse, sizeof(reuse));

    u_long nb = 1;
    ioctlsocket(s_ftp.dataListen, FIONBIO, &nb);

    // Rotate through port range to avoid TIME_WAIT on rapid successive transfers
    int port = FTP_DATA_PORT_BASE + (s_nextDataPort % FTP_DATA_PORT_COUNT);
    s_nextDataPort++;

    SOCKADDR_IN sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons((u_short)port);

    if (bind(s_ftp.dataListen, (SOCKADDR*)&sa, sizeof(sa)) != 0 ||
        listen(s_ftp.dataListen, 1) != 0)
    {
        closesocket(s_ftp.dataListen);
        s_ftp.dataListen = INVALID_SOCKET;
        return false;
    }
    s_ftp.dataPort = (WORD)port;
    return true;
}

// Build PASV response using our IP and data port
static void FtpSendPasv()
{
    // Parse IP octets from s_ipStr
    BYTE oct[4] = { 127, 0, 0, 1 };
    if (s_ipOK)
    {
        const char* p = s_ipStr;
        for (int i = 0; i < 4; ++i)
        {
            int v = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            oct[i] = (BYTE)v;
            if (*p == '.') p++;
        }
    }

    WORD port = s_ftp.dataPort;
    char reply[64];
    char nums[8];
    reply[0] = '\0';
    StrCat2(reply, sizeof(reply), reply, "227 Entering Passive Mode (");

    char o[4];
    for (int i = 0; i < 4; ++i)
    {
        IntToStr((int)oct[i], o, sizeof(o));
        StrCat2(reply, sizeof(reply), reply, o);
        StrCat2(reply, sizeof(reply), reply, ",");
    }
    IntToStr((int)(port >> 8), nums, sizeof(nums));
    StrCat2(reply, sizeof(reply), reply, nums);
    StrCat2(reply, sizeof(reply), reply, ",");
    IntToStr((int)(port & 0xFF), nums, sizeof(nums));
    StrCat2(reply, sizeof(reply), reply, nums);
    StrCat2(reply, sizeof(reply), reply, ").\r\n");
    FtpSendStr(s_ftp.ctrlSock, reply);
}

// Send one LIST line directly to the data socket.
// Uses the real filename — no truncation.
// line format: "drwxr-xr-x  1 xbox xbox  <size> Jan 01 00:00 <name>\r\n"
// Append one LIST line to s_ftp.listBuf. Returns false if buffer full.
static bool FtpAppendListLine(const char* name, bool isDir, DWORD sizeLow)
{
    char line[MAX_PATH_LEN + 64];
    char* p = line;

    const char* perm = isDir ? "drwxr-xr-x" : "-rw-r--r--";
    while (*perm) *p++ = *perm++;
    *p++ = ' ';

    const char* owner = "  1 xbox xbox ";
    while (*owner) *p++ = *owner++;

    char szBuf[12];
    IntToStr((int)sizeLow, szBuf, sizeof(szBuf));
    int szLen = 0; while (szBuf[szLen]) szLen++;
    for (int i = szLen; i < 10; ++i) *p++ = ' ';
    const char* sp = szBuf; while (*sp) *p++ = *sp++;
    *p++ = ' ';

    const char* dt = "Jan 01 00:00 ";
    while (*dt) *p++ = *dt++;

    const char* nm = name; while (*nm) *p++ = *nm++;
    *p++ = '\r'; *p++ = '\n'; *p = '\0';

    int lineLen = (int)(p - line);
    int space = (int)sizeof(s_ftp.listBuf) - s_ftp.listBufLen;
    if (lineLen > space) return false;  // buffer full — truncate listing
    for (int i = 0; i < lineLen; ++i)
        s_ftp.listBuf[s_ftp.listBufLen++] = line[i];
    return true;
}

// ============================================================================
// File operation helpers
// ============================================================================

// Recursively expand srcPath into s_work[], building a flat list of
// WI_MKDIR and WI_FILE entries. dstPath is the destination for srcPath.
static void ExpandToWorkList(const char* srcPath, const char* dstPath, bool isDir)
{
    if (s_workCount >= MAX_WORK_ITEMS) return;

    if (!isDir)
    {
        WorkItem& wi = s_work[s_workCount++];
        wi.type = WI_FILE;
        StrCopy(wi.src, sizeof(wi.src), srcPath);
        StrCopy(wi.dst, sizeof(wi.dst), dstPath);
        return;
    }

    // Emit mkdir for this dir first
    {
        WorkItem& wi = s_work[s_workCount++];
        wi.type = WI_MKDIR;
        wi.src[0] = '\0';
        StrCopy(wi.dst, sizeof(wi.dst), dstPath);
    }

    // Recurse into children
    char pat[MAX_PATH_LEN + 4];
    StrCopy(pat, sizeof(pat), srcPath);
    int pl = 0; while (pat[pl]) pl++;
    if (pl > 0 && pat[pl - 1] != '\\') { pat[pl++] = '\\'; pat[pl] = '\0'; }
    pat[pl++] = '*'; pat[pl] = '\0';

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' || fd.cFileName[1] == '.')) continue;
        if (s_workCount >= MAX_WORK_ITEMS) break;

        char src2[MAX_PATH_LEN], dst2[MAX_PATH_LEN];
        StrCopy(src2, sizeof(src2), srcPath);
        int sl = 0; while (src2[sl]) sl++;
        if (sl > 0 && src2[sl - 1] != '\\') { src2[sl++] = '\\'; src2[sl] = '\0'; }
        AppendStr(src2, sizeof(src2), fd.cFileName);

        StrCopy(dst2, sizeof(dst2), dstPath);
        int dl = 0; while (dst2[dl]) dl++;
        if (dl > 0 && dst2[dl - 1] != '\\') { dst2[dl++] = '\\'; dst2[dl] = '\0'; }
        AppendStr(dst2, sizeof(dst2), fd.cFileName);

        ExpandToWorkList(src2, dst2,
            (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFile(h, &fd));
    FindClose(h);
}

// Delete a file or directory recursively
static bool FileDeleteRecursive(const char* path, bool isDir)
{
    if (!isDir) return DeleteFileA(path) != 0;

    char pat[MAX_PATH_LEN + 4];
    StrCopy(pat, sizeof(pat), path);
    int pl = 0; while (pat[pl]) pl++;
    if (pl > 0 && pat[pl - 1] != '\\') { pat[pl++] = '\\'; pat[pl] = '\0'; }
    pat[pl++] = '*'; pat[pl] = '\0';

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pat, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            if (fd.cFileName[0] == '.' &&
                (fd.cFileName[1] == '\0' || fd.cFileName[1] == '.')) continue;
            char child[MAX_PATH_LEN];
            StrCopy(child, sizeof(child), path);
            int cl = 0; while (child[cl]) cl++;
            if (cl > 0 && child[cl - 1] != '\\') { child[cl++] = '\\'; child[cl] = '\0'; }
            AppendStr(child, sizeof(child), fd.cFileName);
            FileDeleteRecursive(child,
                (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(path) != 0;
}

// Start a file operation — expand clipboard to flat work list, begin ticking.
static void FileOpStart(FileOpType op)
{
    s_workCount = 0;
    s_workIdx = 0;
    s_workOp = op;
    s_opItemDone = 0;
    s_opSrcHandle = INVALID_HANDLE_VALUE;
    s_opDstHandle = INVALID_HANDLE_VALUE;

    for (int i = 0; i < s_clipCount; ++i)
    {
        ClipboardEntry& ce = s_clipboard[i];

        // Build destination: s_path + \ + source filename
        int srcLen = 0; while (ce.path[srcLen]) srcLen++;
        int lastSep = -1;
        for (int k = srcLen - 1; k >= 0; --k)
            if (ce.path[k] == '\\') { lastSep = k; break; }
        const char* fname = (lastSep >= 0) ? ce.path + lastSep + 1 : ce.path;

        char dst[MAX_PATH_LEN];
        StrCopy(dst, sizeof(dst), s_path);
        int dl = 0; while (dst[dl]) dl++;
        if (dl > 0 && dst[dl - 1] != '\\') { dst[dl++] = '\\'; dst[dl] = '\0'; }
        AppendStr(dst, sizeof(dst), fname);

        ExpandToWorkList(ce.path, dst, ce.isDir);
    }

    s_opItemTotal = s_workCount;
    s_opRunning = (s_workCount > 0);
    s_fosState = s_opRunning ? FOS_RUNNING : FOS_IDLE;
}

// Called every tick while s_opRunning. Processes one 64KB chunk per call.
static void FileOpTick()
{
    if (!s_opRunning) return;

    while (s_workIdx < s_workCount)
    {
        WorkItem& wi = s_work[s_workIdx];

        if (wi.type == WI_MKDIR)
        {
            CreateDirectoryA(wi.dst, NULL);
            s_opItemDone++;
            s_workIdx++;
            continue;   // mkdir is instant — loop to next item
        }

        // WI_FILE — open handles on first access
        if (s_opSrcHandle == INVALID_HANDLE_VALUE)
        {
            s_opSrcHandle = CreateFile(wi.src, GENERIC_READ, FILE_SHARE_READ,
                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (s_opSrcHandle == INVALID_HANDLE_VALUE)
            {
                s_opItemDone++; s_workIdx++; continue;
            }

            s_opDstHandle = CreateFile(wi.dst, GENERIC_WRITE, 0,
                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (s_opDstHandle == INVALID_HANDLE_VALUE)
            {
                CloseHandle(s_opSrcHandle);
                s_opSrcHandle = INVALID_HANDLE_VALUE;
                s_opItemDone++; s_workIdx++; continue;
            }

            s_opTotal = GetFileSize(s_opSrcHandle, NULL);
            s_opDone = 0;

            // Display name
            int sl = 0; while (wi.src[sl]) sl++;
            int sep = -1;
            for (int k = sl - 1; k >= 0; --k)
                if (wi.src[k] == '\\') { sep = k; break; }
            TruncName(sep >= 0 ? wi.src + sep + 1 : wi.src,
                s_opSrcName, 18, sizeof(s_opSrcName));
        }

        // One chunk per tick
        static char s_copyBuf[COPY_BUF_SIZE];
        DWORD nr = 0;
        if (!ReadFile(s_opSrcHandle, s_copyBuf, sizeof(s_copyBuf), &nr, NULL) || nr == 0)
        {
            // EOF or error — close handles and advance
            CloseHandle(s_opSrcHandle); s_opSrcHandle = INVALID_HANDLE_VALUE;
            FlushFileBuffers(s_opDstHandle);
            CloseHandle(s_opDstHandle); s_opDstHandle = INVALID_HANDLE_VALUE;
            s_opItemDone++; s_workIdx++;
            break;  // yield — render this frame
        }

        DWORD nw = 0;
        WriteFile(s_opDstHandle, s_copyBuf, nr, &nw, NULL);
        s_opDone += nw;
        break;  // one chunk, then yield
    }

    // All items done
    if (s_workIdx >= s_workCount)
    {
        if (s_opSrcHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(s_opSrcHandle); s_opSrcHandle = INVALID_HANDLE_VALUE;
        }
        if (s_opDstHandle != INVALID_HANDLE_VALUE)
        {
            FlushFileBuffers(s_opDstHandle); CloseHandle(s_opDstHandle); s_opDstHandle = INVALID_HANDLE_VALUE;
        }

        // MOVE: delete sources after successful copy
        if (s_workOp == FILEOP_MOVE)
        {
            for (int i = 0; i < s_clipCount; ++i)
                FileDeleteRecursive(s_clipboard[i].path, s_clipboard[i].isDir);
        }

        s_clipCount = 0;
        s_clipOp = FILEOP_NONE;
        s_opRunning = false;
        s_fosState = FOS_IDLE;
        s_opSrcName[0] = '\0';
        s_workCount = 0;
        s_workIdx = 0;

        LoadDirectory(s_path);
        s_cursor = 0; s_scroll = 0;
    }
}

// Snapshot marked items into clipboard
static void SnapMarkedToClipboard(FileOpType op)
{
    s_clipCount = 0;
    s_clipOp = op;
    for (int i = 0; i < s_entryCount && s_clipCount < MAX_CLIPBOARD; ++i)
    {
        if (!s_marked[i]) continue;
        ClipboardEntry& ce = s_clipboard[s_clipCount++];
        StrCopy(ce.path, sizeof(ce.path), s_path);
        int pl = 0; while (ce.path[pl]) pl++;
        if (pl > 0 && ce.path[pl - 1] != '\\') { ce.path[pl++] = '\\'; ce.path[pl] = '\0'; }
        AppendStr(ce.path, sizeof(ce.path), s_entries[i].name);
        ce.isDir = s_entries[i].isDir;
    }
    for (int i = 0; i < MAX_ENTRIES; ++i) s_marked[i] = false;
    s_markedCount = 0;
}

// Start FTP server
static void FtpStart()
{
    ZeroMemory(&s_ftp, sizeof(s_ftp));
    s_ftp.listenSock = INVALID_SOCKET;
    s_ftp.ctrlSock = INVALID_SOCKET;
    s_ftp.dataListen = INVALID_SOCKET;
    s_ftp.dataSock = INVALID_SOCKET;
    s_ftp.xferFile = INVALID_HANDLE_VALUE;
    s_nextDataPort = 0;  // reset port rotation on each server start

    if (!s_ipOK) return;  // no network

    s_ftp.listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_ftp.listenSock == INVALID_SOCKET) return;

    DWORD reuse = 1;
    setsockopt(s_ftp.listenSock, SOL_SOCKET, SO_REUSEADDR,
        (const char*)&reuse, sizeof(reuse));

    u_long nb = 1;
    ioctlsocket(s_ftp.listenSock, FIONBIO, &nb);

    SOCKADDR_IN sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(FTP_CTRL_PORT);

    if (bind(s_ftp.listenSock, (SOCKADDR*)&sa, sizeof(sa)) != 0 ||
        listen(s_ftp.listenSock, 1) != 0)
    {
        closesocket(s_ftp.listenSock);
        s_ftp.listenSock = INVALID_SOCKET;
        return;
    }

    // Always start in virtual root — MapFtpPath converts at filesystem boundaries
    s_ftp.atVirtualRoot = true;
    StrCopy(s_ftp.cwd, sizeof(s_ftp.cwd), "/");
    s_ftpMsg[0] = '\0';   // clear any "No link" message
    s_ftpMsgFrames = 0;
    s_ftp.state = FTP_LISTEN;
}

// Stop FTP server — close all sockets
static void FtpStop()
{
    if (s_ftp.xferFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(s_ftp.xferFile); s_ftp.xferFile = INVALID_HANDLE_VALUE;
    }
    if (s_ftp.dataSock != INVALID_SOCKET) { closesocket(s_ftp.dataSock);   s_ftp.dataSock = INVALID_SOCKET; }
    if (s_ftp.dataListen != INVALID_SOCKET) { closesocket(s_ftp.dataListen); s_ftp.dataListen = INVALID_SOCKET; }
    if (s_ftp.ctrlSock != INVALID_SOCKET) { closesocket(s_ftp.ctrlSock);   s_ftp.ctrlSock = INVALID_SOCKET; }
    if (s_ftp.listenSock != INVALID_SOCKET) { closesocket(s_ftp.listenSock); s_ftp.listenSock = INVALID_SOCKET; }
    s_ftp.state = FTP_OFF;
}

// Clean a virtual FTP path.
// - Splits on '/' and '\\', processes each component.
// - Skips empty components and ".".
// - ".." pops the last component (clamped at root, cannot escape).
// - Rejects components containing Windows-illegal chars (* ? " < > |).
// - Accepts ANY other name: _folder, .hidden, __temp, F:, etc.
// - Result always starts with '/' and has no trailing '/'.
static void CleanVirtualPath(const char* virtualPath, char* out, int outLen)
{
    int  oi = 0;
    const char* p = virtualPath;

    while (*p)
    {
        // Skip separators
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;

        // Extract next component
        char tok[MAX_PATH_LEN];
        int  ti = 0;
        while (*p && *p != '/' && *p != '\\' && ti < (int)sizeof(tok) - 1)
            tok[ti++] = *p++;
        tok[ti] = '\0';

        if (tok[0] == '\0')
            continue;                               // empty — skip

        if (tok[0] == '.' && tok[1] == '\0')
            continue;                               // "." — skip

        if (tok[0] == '.' && tok[1] == '.' && tok[2] == '\0')
        {
            // ".." — pop last component, clamped at root
            if (oi > 0)
            {
                oi--;
                while (oi > 0 && out[oi] != '/') oi--;
            }
            continue;
        }

        // Reject Windows-illegal chars (colon is allowed: needed for "F:")
        bool bad = false;
        for (int i = 0; tok[i]; i++)
        {
            char c = tok[i];
            if (c == '*' || c == '?' || c == '"' ||
                c == '<' || c == '>' || c == '|')
            {
                bad = true; break;
            }
        }
        if (bad) continue;

        // Append /component
        if (oi < outLen - 1) out[oi++] = '/';
        for (int i = 0; tok[i] && oi < outLen - 1; i++)
            out[oi++] = tok[i];
    }

    if (oi == 0) { out[0] = '/'; out[1] = '\0'; return; }
    out[oi] = '\0';
}


static void ResolveRelative(const char* cwd, const char* arg, char* out, int outLen)
{
    if (arg[0] != '/') {
        // relative — join with backslash then clean
        char tmp[MAX_PATH_LEN * 2];
        StrCopy(tmp, sizeof(tmp), cwd);
        AppendStr(tmp, sizeof(tmp), "\\");
        AppendStr(tmp, sizeof(tmp), arg);
        CleanVirtualPath(tmp, out, outLen);
    }
    else {
        // absolute virtual path — clean as-is
        CleanVirtualPath(arg, out, outLen);
    }
}

// Port of PrometheOS mapFtpPath (simplified for bare drive-letter virtual paths).
// "/F:/Apps/Type D" -> "F:\Apps\Type D"
// "/" -> "/"  (virtual root — caller must check)
static void MapFtpPath(const char* virt, char* out, int outLen)
{
    const char* a = (virt[0] == '/') ? virt + 1 : virt;
    StrCopy(out, outLen, a);
    for (int i = 0; out[i]; i++) if (out[i] == '/') out[i] = '\\';
    // "F:" (drive root) -> "F:\\"
    int len = 0; while (out[len]) len++;
    if (len == 2 && out[1] == ':')
    {
        out[2] = '\\'; out[3] = '\0';
    }
}

// Convenience: resolve arg against cwd then map to real Windows path.
static void FtpResolvePath(const char* arg, char* out, int outLen)
{
    char virt[MAX_PATH_LEN];
    ResolveRelative(s_ftp.cwd, arg, virt, sizeof(virt));
    MapFtpPath(virt, out, outLen);
}
// Execute a pending LIST — enumerate and send directly to dataSock, reply 226.
// Build listing into listBuf then start async drain via XFER_LIST.
// Called from FtpTick once dataSock is accepted.
static void FtpDoListing()
{
    s_ftp.listBufLen = 0;
    s_ftp.listBufOff = 0;

    if (s_ftp.listVirtualRoot)
    {
        const char* driveLetters[] = { "C", "D", "E", "F", "G", "X", "Y", "Z", NULL };
        for (int di = 0; driveLetters[di]; ++di)
        {
            char pat[8];
            pat[0] = driveLetters[di][0]; pat[1] = ':'; pat[2] = '\\'; pat[3] = '*'; pat[4] = '\0';
            WIN32_FIND_DATA fd2;
            HANDLE h2 = FindFirstFile(pat, &fd2);
            if (h2 != INVALID_HANDLE_VALUE)
            {
                FindClose(h2);
                char driveName[4] = { driveLetters[di][0], ':', '\0' };
                FtpAppendListLine(driveName, true, 0);
            }
        }
    }
    else
    {
        char pat[MAX_PATH_LEN + 4];
        StrCopy(pat, sizeof(pat), s_ftp.listDir);
        int pl = 0; while (pat[pl]) pl++;
        if (pl > 0 && pat[pl - 1] != '\\') { pat[pl] = '\\'; pat[pl + 1] = '\0'; pl++; }
        pat[pl] = '*'; pat[pl + 1] = '\0';

        WIN32_FIND_DATA fd;
        HANDLE h = FindFirstFile(pat, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do {
                if (fd.cFileName[0] == '.' && (fd.cFileName[1] == '\0' || fd.cFileName[1] == '.')) continue;
                bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                FtpAppendListLine(fd.cFileName, isDir, isDir ? 0 : fd.nFileSizeLow);
            } while (FindNextFile(h, &fd));
            FindClose(h);
        }
    }

    // Listing built — send 150 and start async drain
    FtpReply(150, "Opening data connection.");
    s_ftp.listPending = false;
    s_ftp.xferType = XFER_LIST;
    s_ftp.state = FTP_TRANSFER;
}

// Recursively delete a directory and all its contents
static bool FtpDeleteDir(const char* path)
{
    char pat[MAX_PATH_LEN + 4];
    StrCopy(pat, sizeof(pat), path);
    int pl = 0; while (pat[pl]) pl++;
    if (pl > 0 && pat[pl - 1] != '\\') { pat[pl] = '\\'; pat[pl + 1] = '\0'; pl++; }
    pat[pl] = '*'; pat[pl + 1] = '\0';

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pat, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            if (fd.cFileName[0] == '.' && (fd.cFileName[1] == '\0' || fd.cFileName[1] == '.')) continue;

            // Build full child path
            char child[MAX_PATH_LEN];
            StrCopy(child, sizeof(child), path);
            int cl = 0; while (child[cl]) cl++;
            if (cl > 0 && child[cl - 1] != '\\') { child[cl] = '\\'; child[cl + 1] = '\0'; }
            AppendStr(child, sizeof(child), fd.cFileName);

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!FtpDeleteDir(child)) { FindClose(h); return false; }
            }
            else
            {
                if (!DeleteFileA(child)) { FindClose(h); return false; }
            }
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(path) != 0;
}

// Process one complete FTP command line (null-terminated, no \r\n)
static void FtpHandleCommand(char* cmd)
{
    // Split verb and argument
    char verb[16] = {};
    char arg[MAX_PATH_LEN] = {};
    int i = 0;
    while (cmd[i] && cmd[i] != ' ' && i < 15) { verb[i] = cmd[i]; i++; }
    verb[i] = '\0';
    if (cmd[i] == ' ') { i++; StrCopy(arg, sizeof(arg), cmd + i); }

    // Strip surrounding quotes — some clients quote paths containing spaces
    // e.g. CWD "F:\Apps\Type D"
    {
        int alen = 0; while (arg[alen]) alen++;
        if (alen >= 2 && arg[0] == '"' && arg[alen - 1] == '"')
        {
            for (int k = 0; k < alen - 2; ++k) arg[k] = arg[k + 1];
            arg[alen - 2] = '\0';
        }
    }

    // Uppercase verb
    for (int k = 0; verb[k]; ++k)
        if (verb[k] >= 'a' && verb[k] <= 'z') verb[k] -= 32;

    if (!s_ftp.authed)
    {
        // Only USER and PASS allowed before auth
        if (verb[0] == 'U' && verb[1] == 'S' && verb[2] == 'E' && verb[3] == 'R')
        {
            // Check username: "xbox"
            bool ok = (arg[0] == 'x' && arg[1] == 'b' && arg[2] == 'o' && arg[3] == 'x' && arg[4] == '\0');
            s_ftp.gotUser = ok;
            FtpReply(331, "Password required.");
        }
        else if (verb[0] == 'P' && verb[1] == 'A' && verb[2] == 'S' && verb[3] == 'S')
        {
            bool ok = s_ftp.gotUser &&
                (arg[0] == 'x' && arg[1] == 'b' && arg[2] == 'o' && arg[3] == 'x' && arg[4] == '\0');
            if (ok) { s_ftp.authed = true; s_ftp.atVirtualRoot = true; StrCopy(s_ftp.cwd, sizeof(s_ftp.cwd), "/"); FtpReply(230, "Logged in."); }
            else { FtpReply(530, "Login incorrect."); }
        }
        else if (verb[0] == 'Q' && verb[1] == 'U' && verb[2] == 'I' && verb[3] == 'T')
        {
            FtpReply(221, "Bye.");
            closesocket(s_ftp.ctrlSock); s_ftp.ctrlSock = INVALID_SOCKET;
            s_ftp.state = FTP_LISTEN;
        }
        else { FtpReply(530, "Not logged in."); }
        return;
    }

    // ---- Authenticated commands ----
    if (verb[0] == 'S' && verb[1] == 'Y' && verb[2] == 'S' && verb[3] == 'T')
    {
        FtpReply(215, "Windows_NT");
    }
    else if (verb[0] == 'T' && verb[1] == 'Y' && verb[2] == 'P' && verb[3] == 'E')
    {
        FtpReply(200, "Type set.");
    }
    else if ((verb[0] == 'P' && verb[1] == 'W' && verb[2] == 'D') ||
        (verb[0] == 'X' && verb[1] == 'P' && verb[2] == 'W' && verb[3] == 'D'))
    {
        // PWD returns virtual path exactly — same as PrometheOS
        char reply[MAX_PATH_LEN + 8];
        reply[0] = '"';
        int ri = 1;
        const char* pp = s_ftp.cwd;
        while (*pp && ri < (int)sizeof(reply) - 4) reply[ri++] = *pp++;
        reply[ri++] = '"'; reply[ri++] = '\0';
        FtpReply(257, reply);
    }
    else if ((verb[0] == 'C' && verb[1] == 'W' && verb[2] == 'D') ||
        (verb[0] == 'X' && verb[1] == 'C' && verb[2] == 'W' && verb[3] == 'D') ||
        (verb[0] == 'C' && verb[1] == 'D' && verb[2] == 'U' && verb[3] == 'P') ||
        (verb[0] == 'X' && verb[1] == 'C' && verb[2] == 'U' && verb[3] == 'P'))
    {
        // Exact port of PrometheOS CWD/CDUP logic.
        bool isCdup = (verb[0] == 'C' && verb[1] == 'D' && verb[2] == 'U') ||
            (verb[0] == 'X' && verb[1] == 'C' && verb[2] == 'U');
        const char* param = isCdup ? ".." : arg;

        char newVirtual[MAX_PATH_LEN];
        ResolveRelative(s_ftp.cwd, param, newVirtual, sizeof(newVirtual));

        bool isFolder = false;

        // Case 1: resolved to virtual root "/"
        if (newVirtual[0] == '/' && newVirtual[1] == '\0')
        {
            isFolder = true;
        }
        else
        {
            char ftpPath[MAX_PATH_LEN];
            MapFtpPath(newVirtual, ftpPath, sizeof(ftpPath));

            // Case 2: drive root — ftpPath is "F:" or "F:\"
            int fplen = 0; while (ftpPath[fplen]) fplen++;
            if (fplen >= 2 && ftpPath[1] == ':' && (ftpPath[2] == '\0' || (ftpPath[2] == '\\' && ftpPath[3] == '\0')))
            {
                isFolder = true;
            }
            else
            {
                // Case 3: subdirectory.
                // GetFileAttributesA first — works on non-empty dirs.
                // Fallback: try CreateDirectoryA; if it fails with
                // ERROR_ALREADY_EXISTS the path exists and is a directory.
                // This handles empty directories on RXDK where GetFileAttributes
                // may return INVALID_FILE_ATTRIBUTES.
                DWORD attr = GetFileAttributesA(ftpPath);
                if (attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY))
                {
                    isFolder = true;
                }
                else
                {
                    if (!CreateDirectoryA(ftpPath, NULL) &&
                        GetLastError() == ERROR_ALREADY_EXISTS)
                        isFolder = true;
                }
            }
        }

        if (isFolder)
        {
            s_ftp.atVirtualRoot = (newVirtual[0] == '/' && newVirtual[1] == '\0');
            StrCopy(s_ftp.cwd, sizeof(s_ftp.cwd), newVirtual);
            FtpReply(250, "Directory changed.");
        }
        else
        {
            FtpReply(550, "No such directory.");
        }
    }
    else if (verb[0] == 'P' && verb[1] == 'A' && verb[2] == 'S' && verb[3] == 'V')
    {
        if (FtpOpenPassive()) FtpSendPasv();
        else FtpReply(425, "Cannot open data connection.");
    }
    else if ((verb[0] == 'L' && verb[1] == 'I' && verb[2] == 'S' && verb[3] == 'T') ||
        (verb[0] == 'N' && verb[1] == 'L' && verb[2] == 'S' && verb[3] == 'T'))
    {
        if (s_ftp.dataListen == INVALID_SOCKET)
        {
            FtpReply(425, "Use PASV first."); return;
        }

        // Strip all leading flag groups (e.g. "-a", "-la", "-al", "-a -l")
        // Some clients send multiple flag tokens before the path or nothing.
        char* listArg = arg;
        while (listArg[0] == '-')
        {
            while (*listArg && *listArg != ' ') listArg++;  // skip flag token
            while (*listArg == ' ') listArg++;              // skip spaces
        }

        // Resolve listing target.
        // Empty arg or bare "/" after flag stripping always means current directory.
        // At virtual root that produces the drive letter listing.
        bool isRootArg = (listArg[0] == '\0') ||
            (listArg[0] == '/' && listArg[1] == '\0');
        bool listVirtualRoot = s_ftp.atVirtualRoot && isRootArg;
        char listDir[MAX_PATH_LEN] = {};
        if (!listVirtualRoot)
        {
            if (listArg[0] != '\0')
                FtpResolvePath(listArg, listDir, sizeof(listDir));
            else
                MapFtpPath(s_ftp.cwd, listDir, sizeof(listDir));
        }

        // Store as pending then dispatch immediately if dataSock is already
        // accepted (fast clients connect to the data port before LIST arrives).
        // Otherwise FtpTick dispatches it when accept fires.
        s_ftp.listPending = true;
        s_ftp.listVirtualRoot = listVirtualRoot;
        StrCopy(s_ftp.listDir, sizeof(s_ftp.listDir), listDir);

        if (s_ftp.dataSock != INVALID_SOCKET)
            FtpDoListing();
    }
    else if (verb[0] == 'R' && verb[1] == 'E' && verb[2] == 'T' && verb[3] == 'R')
    {
        if (s_ftp.dataListen == INVALID_SOCKET)
        {
            FtpReply(425, "Use PASV first."); return;
        }

        char fullPath[MAX_PATH_LEN];
        FtpResolvePath(arg, fullPath, sizeof(fullPath));

        HANDLE hf = CreateFile(fullPath, GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE)
        {
            FtpReply(550, "File not found."); return;
        }

        s_ftp.xferFile = hf;
        s_ftp.xferTotal = GetFileSize(hf, NULL);
        s_ftp.xferDone = 0;
        s_ftp.retrBufLen = 0;
        s_ftp.retrBufOff = 0;
        TruncName(arg, s_ftp.xferName, 18, sizeof(s_ftp.xferName));
        StrCopy(s_ftp.xferPath, sizeof(s_ftp.xferPath), fullPath);
        FtpReply(150, "Opening data connection.");
        s_ftp.xferType = XFER_RETR;
        s_ftp.state = FTP_TRANSFER;
    }
    else if (verb[0] == 'S' && verb[1] == 'T' && verb[2] == 'O' && verb[3] == 'R')
    {
        if (s_ftp.dataListen == INVALID_SOCKET)
        {
            FtpReply(425, "Use PASV first."); return;
        }

        char fullPath[MAX_PATH_LEN];
        FtpResolvePath(arg, fullPath, sizeof(fullPath));

        HANDLE hf = CreateFile(fullPath, GENERIC_WRITE, 0,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE)
        {
            FtpReply(550, "Cannot create file."); return;
        }

        s_ftp.xferFile = hf;
        s_ftp.xferTotal = 0;   // unknown until complete
        s_ftp.xferDone = 0;
        TruncName(arg, s_ftp.xferName, 18, sizeof(s_ftp.xferName));
        StrCopy(s_ftp.xferPath, sizeof(s_ftp.xferPath), fullPath);
        FtpReply(150, "Opening data connection.");
        s_ftp.xferType = XFER_STOR;
        s_ftp.state = FTP_TRANSFER;
    }
    else if (verb[0] == 'Q' && verb[1] == 'U' && verb[2] == 'I' && verb[3] == 'T')
    {
        FtpReply(221, "Bye.");
        if (s_ftp.xferFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(s_ftp.xferFile); s_ftp.xferFile = INVALID_HANDLE_VALUE;
        }
        if (s_ftp.dataSock != INVALID_SOCKET) { closesocket(s_ftp.dataSock);   s_ftp.dataSock = INVALID_SOCKET; }
        if (s_ftp.dataListen != INVALID_SOCKET) { closesocket(s_ftp.dataListen); s_ftp.dataListen = INVALID_SOCKET; }
        closesocket(s_ftp.ctrlSock); s_ftp.ctrlSock = INVALID_SOCKET;
        s_ftp.state = FTP_LISTEN;
        s_ftp.authed = false;
        s_ftp.gotUser = false;
        s_ftp.gotRnfr = false;
        s_ftp.atVirtualRoot = false;
        s_ftp.listPending = false;
        s_ftp.xferType = XFER_NONE;
        s_ftp.recvLen = 0;
        s_ftp.listBufLen = 0;
        s_ftp.listBufOff = 0;
        s_ftp.sendLen = 0;
        s_ftp.sendOff = 0;
    }
    else if (verb[0] == 'D' && verb[1] == 'E' && verb[2] == 'L' && verb[3] == 'E')
    {
        // Delete a file
        char fullPath[MAX_PATH_LEN];
        FtpResolvePath(arg, fullPath, sizeof(fullPath));
        if (DeleteFileA(fullPath))
            FtpReply(250, "File deleted.");
        else
            FtpReply(550, "Delete failed.");
    }
    else if ((verb[0] == 'M' && verb[1] == 'K' && verb[2] == 'D') ||
        (verb[0] == 'X' && verb[1] == 'M' && verb[2] == 'K' && verb[3] == 'D'))
    {
        // Create directory
        char newVirt[MAX_PATH_LEN];
        ResolveRelative(s_ftp.cwd, arg, newVirt, sizeof(newVirt));
        char fullPath[MAX_PATH_LEN];
        MapFtpPath(newVirt, fullPath, sizeof(fullPath));
        if (CreateDirectoryA(fullPath, NULL))
        {
            // RFC 959: 257 response quotes the virtual path
            char reply[MAX_PATH_LEN + 4];
            reply[0] = '"';
            int ri = 1;
            const char* pp = newVirt;
            while (*pp && ri < (int)sizeof(reply) - 3) reply[ri++] = *pp++;
            reply[ri++] = '"'; reply[ri] = '\0';
            FtpReply(257, reply);
        }
        else
            FtpReply(550, "Cannot create directory.");
    }
    else if ((verb[0] == 'R' && verb[1] == 'M' && verb[2] == 'D') ||
        (verb[0] == 'X' && verb[1] == 'R' && verb[2] == 'M' && verb[3] == 'D'))
    {
        // Remove directory and all contents recursively
        char fullPath[MAX_PATH_LEN];
        FtpResolvePath(arg, fullPath, sizeof(fullPath));
        if (FtpDeleteDir(fullPath))
            FtpReply(250, "Directory removed.");
        else
            FtpReply(550, "Remove failed.");
    }
    else if (verb[0] == 'R' && verb[1] == 'N' && verb[2] == 'F' && verb[3] == 'R')
    {
        // Rename from — verify source exists before storing
        char fullPath[MAX_PATH_LEN];
        FtpResolvePath(arg, fullPath, sizeof(fullPath));

        // Check as file first, then as directory
        DWORD attr = GetFileAttributesA(fullPath);
        if (attr == 0xFFFFFFFF)
        {
            s_ftp.gotRnfr = false;
            FtpReply(550, "No such file or directory.");
        }
        else
        {
            StrCopy(s_ftp.rnfrPath, sizeof(s_ftp.rnfrPath), fullPath);
            s_ftp.gotRnfr = true;
            FtpReply(350, "Waiting for RNTO.");
        }
    }
    else if (verb[0] == 'R' && verb[1] == 'N' && verb[2] == 'T' && verb[3] == 'O')
    {
        if (!s_ftp.gotRnfr)
        {
            FtpReply(503, "Send RNFR first."); return;
        }
        s_ftp.gotRnfr = false;

        char fullPath[MAX_PATH_LEN];
        FtpResolvePath(arg, fullPath, sizeof(fullPath));
        if (MoveFileA(s_ftp.rnfrPath, fullPath))
            FtpReply(250, "Renamed.");
        else
            FtpReply(550, "Rename failed.");
    }
    else if (verb[0] == 'F' && verb[1] == 'E' && verb[2] == 'A' && verb[3] == 'T')
    {
        // Advertise SIZE so clients know file sizes before transfer
        FtpSendStr(s_ftp.ctrlSock, "211-Features:\r\n SIZE\r\n TVFS\r\n211 END\r\n");
    }
    else if (verb[0] == 'O' && verb[1] == 'P' && verb[2] == 'T' && verb[3] == 'S')
    {
        // FileZilla sends "OPTS UTF8 ON" — just accept it
        FtpReply(200, "OK.");
    }
    else if (verb[0] == 'N' && verb[1] == 'O' && verb[2] == 'O' && verb[3] == 'P')
    {
        FtpReply(200, "OK.");
    }
    else if (verb[0] == 'S' && verb[1] == 'I' && verb[2] == 'Z' && verb[3] == 'E')
    {
        // Return file size so client can show progress
        char fullPath[MAX_PATH_LEN];
        FtpResolvePath(arg, fullPath, sizeof(fullPath));
        HANDLE hf = CreateFile(fullPath, GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE)
        {
            DWORD sz = GetFileSize(hf, NULL);
            CloseHandle(hf);
            char szBuf[32];
            char reply[48];
            IntToStr((int)sz, szBuf, sizeof(szBuf));
            reply[0] = '\0';
            StrCat2(reply, sizeof(reply), reply, szBuf);
            FtpReply(213, reply);
        }
        else { FtpReply(550, "File not found."); }
    }
    else
    {
        FtpReply(502, "Command not implemented.");
    }
}

// ============================================================================
// FtpTick — called once per frame, non-blocking throughout
// ============================================================================

static void FtpTick()
{
    if (s_ftp.state == FTP_OFF) return;

    // ---- Drain ctrl send buffer ------------------------------------------
    // FtpSendStr() queues into sendBuf; we push bytes here each tick so
    // WSAEWOULDBLOCK never silently drops part of a reply.
    if (s_ftp.ctrlSock != INVALID_SOCKET && s_ftp.sendLen > s_ftp.sendOff)
    {
        int n = send(s_ftp.ctrlSock,
            s_ftp.sendBuf + s_ftp.sendOff,
            s_ftp.sendLen - s_ftp.sendOff, 0);
        if (n > 0)
        {
            s_ftp.sendOff += n;
            if (s_ftp.sendOff >= s_ftp.sendLen)
                s_ftp.sendOff = s_ftp.sendLen = 0;  // buffer fully drained
        }
        else if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
        {
            // Hard send error on ctrl socket — treat as disconnect
            closesocket(s_ftp.ctrlSock); s_ftp.ctrlSock = INVALID_SOCKET;
            if (s_ftp.xferFile != INVALID_HANDLE_VALUE)
            {
                CloseHandle(s_ftp.xferFile); s_ftp.xferFile = INVALID_HANDLE_VALUE;
            }
            if (s_ftp.dataSock != INVALID_SOCKET) { closesocket(s_ftp.dataSock);   s_ftp.dataSock = INVALID_SOCKET; }
            if (s_ftp.dataListen != INVALID_SOCKET) { closesocket(s_ftp.dataListen); s_ftp.dataListen = INVALID_SOCKET; }
            s_ftp.authed = false; s_ftp.gotUser = false; s_ftp.gotRnfr = false;
            s_ftp.atVirtualRoot = false; s_ftp.listPending = false;
            s_ftp.xferType = XFER_NONE; s_ftp.recvLen = 0;
            s_ftp.retrBufLen = 0; s_ftp.retrBufOff = 0;
            s_ftp.listBufLen = 0; s_ftp.listBufOff = 0;
            s_ftp.sendLen = 0; s_ftp.sendOff = 0;
            s_ftp.state = FTP_LISTEN;
            return;
        }
    }

    // ---- Accept new control connection ------------------------------------
    if (s_ftp.state == FTP_LISTEN && s_ftp.listenSock != INVALID_SOCKET)
    {
        SOCKADDR_IN ca; int caLen = sizeof(ca);
        SOCKET cs = accept(s_ftp.listenSock, (SOCKADDR*)&ca, &caLen);
        if (cs != INVALID_SOCKET)
        {
            u_long nb = 1; ioctlsocket(cs, FIONBIO, &nb);
            s_ftp.ctrlSock = cs;
            s_ftp.authed = false;
            s_ftp.gotUser = false;
            s_ftp.gotRnfr = false;
            s_ftp.atVirtualRoot = false;
            s_ftp.xferType = XFER_NONE;
            s_ftp.recvLen = 0;
            s_ftp.state = FTP_CONNECTED;
            FtpReply(220, "XbDiag FTP ready.");
        }
        return;
    }

    // ---- Accept passive data connection ----------------------------------
    // Try to accept whenever dataListen is open and we don't have dataSock yet.
    if (s_ftp.dataSock == INVALID_SOCKET &&
        s_ftp.dataListen != INVALID_SOCKET &&
        (s_ftp.state == FTP_CONNECTED || s_ftp.state == FTP_TRANSFER))
    {
        SOCKADDR_IN da; int daLen = sizeof(da);
        SOCKET ds = accept(s_ftp.dataListen, (SOCKADDR*)&da, &daLen);
        if (ds != INVALID_SOCKET)
        {
            u_long nb = 1; ioctlsocket(ds, FIONBIO, &nb);
            s_ftp.dataSock = ds;

            // If a LIST was waiting for a data connection, execute it now
            if (s_ftp.listPending)
                FtpDoListing();
        }
    }

    // Fallback: listPending but dataSock already accepted (fast client connected
    // to data port before LIST command arrived — dispatch was handled inline in
    // FtpHandleCommand, but catch any edge case where it was missed).
    if (s_ftp.listPending &&
        s_ftp.dataSock != INVALID_SOCKET &&
        s_ftp.xferType == XFER_NONE)
    {
        FtpDoListing();
    }

    // ---- Control socket: receive and command dispatch --------------------
    if ((s_ftp.state == FTP_CONNECTED || s_ftp.state == FTP_TRANSFER) &&
        s_ftp.ctrlSock != INVALID_SOCKET)
    {
        // Receive new data only when the send buffer has room — backpressure.
        // Disconnect detection always runs regardless of send buffer state.
        const int SEND_BUF_PAUSE_THRESHOLD = (int)sizeof(s_ftp.sendBuf) - 256;
        if (s_ftp.sendLen < SEND_BUF_PAUSE_THRESHOLD)
        {
            int space = (int)sizeof(s_ftp.recvBuf) - s_ftp.recvLen - 1;
            if (space > 0)
            {
                int n = recv(s_ftp.ctrlSock,
                    s_ftp.recvBuf + s_ftp.recvLen, space, 0);
                if (n > 0) s_ftp.recvLen += n;
                else if (n == 0 || (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK))
                    goto ctrl_disconnect;
            }
            else
            {
                // recvBuf full — discard up to next newline, keep remainder
                int nl = -1;
                for (int i = 0; i < s_ftp.recvLen; ++i)
                    if (s_ftp.recvBuf[i] == '\n') { nl = i; break; }
                if (nl >= 0)
                {
                    int remaining = s_ftp.recvLen - nl - 1;
                    for (int i = 0; i < remaining; ++i)
                        s_ftp.recvBuf[i] = s_ftp.recvBuf[nl + 1 + i];
                    s_ftp.recvLen = remaining;
                }
                else s_ftp.recvLen = 0;
            }
        }
        else
        {
            // Send buffer near full — skip recv this tick, drain loop will
            // clear space. Commands already in recvBuf still get parsed below.
        }

        // Parse and dispatch all complete commands already in recvBuf.
        // This runs regardless of send buffer state — commands buffered before
        // the gate closed still need to be processed.
        {
            s_ftp.recvBuf[s_ftp.recvLen] = '\0';
            char* buf = s_ftp.recvBuf;
            while (true)
            {
                int pos = -1;
                for (int i = 0; i < s_ftp.recvLen; ++i)
                    if (buf[i] == '\n') { pos = i; break; }
                if (pos < 0) break;

                if (pos > 0 && buf[pos - 1] == '\r') buf[pos - 1] = '\0';
                buf[pos] = '\0';

                FtpHandleCommand(buf);

                int remaining = s_ftp.recvLen - pos - 1;
                for (int i = 0; i < remaining; ++i) buf[i] = buf[pos + 1 + i];
                s_ftp.recvLen = remaining;
                buf[remaining] = '\0';
            }
        }
        goto skip_ctrl_disconnect;

    ctrl_disconnect:
        {
            // Client disconnected or hard socket error — full session teardown
            if (s_ftp.xferFile != INVALID_HANDLE_VALUE)
            {
                CloseHandle(s_ftp.xferFile); s_ftp.xferFile = INVALID_HANDLE_VALUE;
            }
            if (s_ftp.dataSock != INVALID_SOCKET) { closesocket(s_ftp.dataSock);   s_ftp.dataSock = INVALID_SOCKET; }
            if (s_ftp.dataListen != INVALID_SOCKET) { closesocket(s_ftp.dataListen); s_ftp.dataListen = INVALID_SOCKET; }
            closesocket(s_ftp.ctrlSock); s_ftp.ctrlSock = INVALID_SOCKET;
            s_ftp.authed = false;
            s_ftp.gotUser = false;
            s_ftp.gotRnfr = false;
            s_ftp.atVirtualRoot = false;
            s_ftp.listPending = false;
            s_ftp.xferType = XFER_NONE;
            s_ftp.recvLen = 0;
            s_ftp.retrBufLen = 0;
            s_ftp.retrBufOff = 0;
            s_ftp.listBufLen = 0;
            s_ftp.listBufOff = 0;
            s_ftp.sendLen = 0;
            s_ftp.sendOff = 0;
            s_ftp.state = FTP_LISTEN;
            return;
        }
    skip_ctrl_disconnect:;
    }

    // ---- Data transfer ---------------------------------------------------
    if (s_ftp.state == FTP_TRANSFER && s_ftp.dataSock != INVALID_SOCKET)
    {
        static char ioBuf[4096];

        if (s_ftp.xferType == XFER_LIST)
        {
            // Drain listBuf into dataSock non-blockingly, same pattern as RETR
            if (s_ftp.listBufOff < s_ftp.listBufLen)
            {
                int toSend = s_ftp.listBufLen - s_ftp.listBufOff;
                int sent = send(s_ftp.dataSock,
                    s_ftp.listBuf + s_ftp.listBufOff, toSend, 0);
                if (sent > 0)
                {
                    s_ftp.listBufOff += sent;
                }
                else if (sent < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
                {
                    // Data socket died mid-listing
                    closesocket(s_ftp.dataSock); s_ftp.dataSock = INVALID_SOCKET;
                    FtpReply(426, "Connection closed; transfer aborted.");
                    s_ftp.xferType = XFER_NONE;
                    s_ftp.state = FTP_CONNECTED;
                    return;
                }
                // WSAEWOULDBLOCK — just wait for next tick
            }
            else
            {
                // All listing data sent — close data socket and reply 226
                closesocket(s_ftp.dataSock); s_ftp.dataSock = INVALID_SOCKET;
                FtpReply(226, "Transfer complete.");
                s_ftp.xferType = XFER_NONE;
                s_ftp.state = FTP_CONNECTED;
            }
        }
        else if (s_ftp.xferType == XFER_RETR)
        {
            // If we have unsent data from last tick, drain it first
            if (s_ftp.retrBufOff < s_ftp.retrBufLen)
            {
                int toSend = s_ftp.retrBufLen - s_ftp.retrBufOff;
                int sent = send(s_ftp.dataSock,
                    s_ftp.retrBuf + s_ftp.retrBufOff, toSend, 0);
                if (sent > 0)
                {
                    s_ftp.retrBufOff += sent;
                    s_ftp.xferDone += (DWORD)sent;
                }
                else if (sent < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
                {
                    // Client aborted
                    closesocket(s_ftp.dataSock); s_ftp.dataSock = INVALID_SOCKET;
                    CloseHandle(s_ftp.xferFile); s_ftp.xferFile = INVALID_HANDLE_VALUE;
                    FtpReply(426, "Transfer aborted.");
                    s_ftp.xferType = XFER_NONE;
                    s_ftp.state = FTP_CONNECTED;
                }
                // Either drained or WSAEWOULDBLOCK — don't read more this tick
                return;
            }

            // Buffer empty — read next chunk from file
            s_ftp.retrBufLen = 0;
            s_ftp.retrBufOff = 0;
            DWORD bytesRead = 0;
            if (ReadFile(s_ftp.xferFile, s_ftp.retrBuf, sizeof(s_ftp.retrBuf),
                &bytesRead, NULL) && bytesRead > 0)
            {
                s_ftp.retrBufLen = (int)bytesRead;
                // Don't send here — let next tick's drain path handle it
                // (avoids double-tick issue; drain runs at top of next frame)
                int sent = send(s_ftp.dataSock, s_ftp.retrBuf, s_ftp.retrBufLen, 0);
                if (sent > 0)
                {
                    s_ftp.retrBufOff += sent;
                    s_ftp.xferDone += (DWORD)sent;
                }
                else if (sent < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
                {
                    closesocket(s_ftp.dataSock); s_ftp.dataSock = INVALID_SOCKET;
                    CloseHandle(s_ftp.xferFile); s_ftp.xferFile = INVALID_HANDLE_VALUE;
                    FtpReply(426, "Transfer aborted.");
                    s_ftp.xferType = XFER_NONE;
                    s_ftp.state = FTP_CONNECTED;
                }
                // Partial or WSAEWOULDBLOCK: retrBufOff < retrBufLen,
                // remainder drained next tick
            }
            else
            {
                // EOF or read error — transfer complete
                closesocket(s_ftp.dataSock); s_ftp.dataSock = INVALID_SOCKET;
                // dataListen stays open for the next PASV/transfer
                CloseHandle(s_ftp.xferFile); s_ftp.xferFile = INVALID_HANDLE_VALUE;
                FtpReply(226, "Transfer complete.");
                s_ftp.xferType = XFER_NONE;
                s_ftp.state = FTP_CONNECTED;
            }
        }
        else if (s_ftp.xferType == XFER_STOR)
        {
            // Receive chunk from client, write to file
            int n = recv(s_ftp.dataSock, ioBuf, sizeof(ioBuf), 0);
            if (n > 0)
            {
                DWORD written = 0;
                if (!WriteFile(s_ftp.xferFile, ioBuf, (DWORD)n, &written, NULL) ||
                    written != (DWORD)n)
                {
                    // Disk full or write error — abort
                    closesocket(s_ftp.dataSock); s_ftp.dataSock = INVALID_SOCKET;
                    CloseHandle(s_ftp.xferFile); s_ftp.xferFile = INVALID_HANDLE_VALUE;
                    FtpReply(452, "Write error - disk full?");
                    s_ftp.xferType = XFER_NONE;
                    s_ftp.state = FTP_CONNECTED;
                    return;
                }
                s_ftp.xferDone += written;
            }
            else if (n == 0)
            {
                // Client closed data connection = transfer complete
                FlushFileBuffers(s_ftp.xferFile);
                closesocket(s_ftp.dataSock); s_ftp.dataSock = INVALID_SOCKET;
                // dataListen stays open for the next PASV/transfer
                CloseHandle(s_ftp.xferFile); s_ftp.xferFile = INVALID_HANDLE_VALUE;
                FtpReply(226, "Transfer complete.");
                s_ftp.xferType = XFER_NONE;
                s_ftp.state = FTP_CONNECTED;
            }
            else if (WSAGetLastError() != WSAEWOULDBLOCK)
            {
                // Hard recv error — client aborted
                closesocket(s_ftp.dataSock); s_ftp.dataSock = INVALID_SOCKET;
                CloseHandle(s_ftp.xferFile); s_ftp.xferFile = INVALID_HANDLE_VALUE;
                FtpReply(426, "Transfer aborted.");
                s_ftp.xferType = XFER_NONE;
                s_ftp.state = FTP_CONNECTED;
            }
            // WSAEWOULDBLOCK: nothing arrived this frame, retry next frame
        }
    }
}

// ============================================================================
// FTP widget render
// ============================================================================

static void DrawFtpWidget()
{
    if (s_ftp.state == FTP_OFF) return;

    bool transferring = (s_ftp.state == FTP_TRANSFER &&
        s_ftp.xferType != XFER_NONE);

    // Compute widget height
    float lineCount = 2.f;                         // status + ip
    if (transferring) lineCount += 2.f;            // filename + progress bar
    float wh = WIDGET_PAD * 2.f + lineCount * WIDGET_LINE_H;
    float wy = BOT_BAR_Y - wh - 4.f;
    float wx = WIDGET_X;
    float ww = WIDGET_W;

    // Background + border
    FillRect(wx, wy, wx + ww, wy + wh, D3DCOLOR_ARGB(210, 8, 12, 30));
    HLine(wy, wx, wx + ww, COL_CYAN);
    HLine(wy + wh, wx, wx + ww, COL_CYAN);
    VLine(wx, wy, wy + wh, COL_BORDER);
    VLine(wx + ww, wy, wy + wh, COL_BORDER);

    float ty = wy + WIDGET_PAD;

    // Status line
    const char* stateStr;
    DWORD dotCol;
    switch (s_ftp.state)
    {
    case FTP_LISTEN:    stateStr = "LISTENING";   dotCol = COL_YELLOW; break;
    case FTP_CONNECTED: stateStr = "CONNECTED";   dotCol = COL_GREEN;  break;
    case FTP_TRANSFER:  stateStr = "TRANSFER";    dotCol = COL_CYAN;   break;
    default:            stateStr = "OFF";         dotCol = COL_GRAY;   break;
    }

    FillRect(wx + WIDGET_PAD, ty + 3.f, wx + WIDGET_PAD + 8.f, ty + 11.f, dotCol);
    DrawText(wx + WIDGET_PAD + 12.f, ty, "FTP", 1.15f, COL_WHITE);
    DrawText(wx + WIDGET_PAD + 38.f, ty, stateStr, 1.15f, dotCol);
    ty += WIDGET_LINE_H;

    // IP + port
    char ipPort[32];
    StrCopy(ipPort, sizeof(ipPort), s_ipStr);
    AppendStr(ipPort, sizeof(ipPort), " :21");
    DrawText(wx + WIDGET_PAD, ty, ipPort, 1.05f, COL_CYAN);
    ty += WIDGET_LINE_H;

    if (transferring)
    {
        // Transfer verb + filename
        const char* verb = (s_ftp.xferType == XFER_RETR) ? "GET " :
            (s_ftp.xferType == XFER_STOR) ? "PUT " : "??? ";
        char dispLine[32];
        StrCopy(dispLine, sizeof(dispLine), verb);
        AppendStr(dispLine, sizeof(dispLine), s_ftp.xferName);
        DrawText(wx + WIDGET_PAD, ty, dispLine, 1.05f, COL_WHITE);
        ty += WIDGET_LINE_H;

        // Progress bar — indeterminate for STOR (size unknown), determinate for RETR
        const float BAR_W = ww - WIDGET_PAD * 2.f - 32.f;
        const float BAR_H = 8.f;
        float frac = 0.f;
        char pctBuf[8] = "...";

        if (s_ftp.xferType == XFER_RETR && s_ftp.xferTotal > 0)
        {
            frac = (float)s_ftp.xferDone / (float)s_ftp.xferTotal;
            if (frac > 1.f) frac = 1.f;
            int pct = Ftoi(frac * 100.f);
            IntToStr(pct, pctBuf, sizeof(pctBuf));
            AppendStr(pctBuf, sizeof(pctBuf), "%");
        }
        else if (s_ftp.xferType == XFER_STOR)
        {
            // Animate: crawling block
            DWORD t = GetTickCount();
            frac = (float)((t / 50) % 100) / 100.f;
        }
        else
            frac = 0.5f;  // LIST: just show half-full

        FillRect(wx + WIDGET_PAD, ty + 1.f, wx + WIDGET_PAD + BAR_W, ty + BAR_H + 1.f,
            D3DCOLOR_XRGB(15, 20, 50));
        FillRectGrad(wx + WIDGET_PAD, ty + 1.f, wx + WIDGET_PAD + BAR_W * frac, ty + BAR_H + 1.f,
            D3DCOLOR_XRGB(60, 180, 255), D3DCOLOR_XRGB(20, 80, 160));
        HLine(ty + 1.f, wx + WIDGET_PAD, wx + WIDGET_PAD + BAR_W, COL_BORDER);
        HLine(ty + BAR_H + 1.f, wx + WIDGET_PAD, wx + WIDGET_PAD + BAR_W, COL_BORDER);
        DrawText(wx + WIDGET_PAD + BAR_W + 4.f, ty, pctBuf, 1.05f, COL_WHITE);
    }
}

// ============================================================================
// OnEnter
// ============================================================================

void FileExplorer_OnEnter()
{
    s_prevBtns = 0;
    s_skipFirstTick = true;

    // Start network stack (ref-counted — safe if SysInfo already called it)
    XNetStartupParams xnsp;
    ZeroMemory(&xnsp, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    XNetStartup(&xnsp);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    ResolveIP();
    LoadDriveList();

    // FTP off until [Start]
    ZeroMemory(&s_ftp, sizeof(s_ftp));
    s_ftp.listenSock = INVALID_SOCKET;
    s_ftp.ctrlSock = INVALID_SOCKET;
    s_ftp.dataListen = INVALID_SOCKET;
    s_ftp.dataSock = INVALID_SOCKET;
    s_ftp.xferFile = INVALID_HANDLE_VALUE;
    s_ftp.state = FTP_OFF;
    s_ftpMsg[0] = '\0';
    s_ftpMsgFrames = 0;
}

// ============================================================================
// Render
// ============================================================================

static void Render(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    // Build hint string — suppress [X] Launch hint when not applicable
    const char* hints;
    bool canLaunch = !s_atRoot && s_entryCount > 0 &&
        !s_entries[s_cursor].isDir &&
        IsXBE(s_entries[s_cursor].name) &&
        s_markedCount == 0;
    if (s_fosState == FOS_CONFIRM_DELETE)
        hints = "[B] Confirm Delete  [Back] Cancel";
    else if (s_markedCount > 0)
        hints = "[Y] Mark  [Black] Copy  [White] Move  [B] Delete  [Back] Clear";
    else if (s_clipCount > 0)
        hints = "[Black] Paste  [White] Move here  [Back] Clear clipboard";
    else if (s_ftp.state != FTP_OFF)
        hints = canLaunch ? "[A] Open  [B] Back  [X] Launch  [Y] Mark  [Start] FTP Off"
        : "[A] Open  [B] Back  [Y] Mark  [Start] FTP Off";
    else
        hints = canLaunch ? "[A] Open  [B] Back  [X] Launch  [Y] Mark  [Start] FTP On"
        : "[A] Open  [B] Back  [Y] Mark  [Start] FTP On";

    DrawPageChrome(logo, "FILE EXPLORER", hints);

    // ---- Path bar --------------------------------------------------------
    FillRect(0.f, CONTENT_Y, SW, LIST_Y, D3DCOLOR_XRGB(12, 15, 35));
    HLine(LIST_Y, 0.f, SW, COL_BORDER);

    const char* pathDisp = s_atRoot ? "DRIVES" : s_path;
    DrawText(LM, PATH_Y, pathDisp, 1.2f, COL_CYAN);

    // FTP status indicator in path bar (right-aligned, compact)
    if (s_ftp.state != FTP_OFF)
    {
        char ftpBadge[32];
        StrCopy(ftpBadge, sizeof(ftpBadge), "FTP ");
        AppendStr(ftpBadge, sizeof(ftpBadge), s_ipStr);
        DrawTextR(SW - LM, PATH_Y, ftpBadge, 1.1f,
            s_ftp.state == FTP_TRANSFER ? COL_CYAN : COL_GREEN);
    }
    else if (s_ftpMsgFrames > 0)
    {
        // Briefly show reason FTP couldn't start (e.g. "No Ethernet link")
        DrawTextR(SW - LM, PATH_Y, s_ftpMsg, 1.1f, D3DCOLOR_XRGB(255, 80, 80));
        s_ftpMsgFrames--;
    }

    // ---- File list -------------------------------------------------------
    const float ICON_X = LM;
    const float NAME_X = LM + 16.f;
    const float SIZE_X = SW - LM - WIDGET_W - 12.f;  // leave room for widget
    const float NAME_W = SIZE_X - NAME_X - 8.f;       // available name width
    // Approx chars that fit in NAME_W at scale 1.2 (each char ~7px)
    const int   NAME_MAX_CHARS = (int)(NAME_W / 7.f);

    for (int i = 0; i < ROWS_VISIBLE; ++i)
    {
        int idx = s_scroll + i;
        float ry = LIST_Y + (float)i * ROW_H;

        if (idx >= s_entryCount)
            break;

        bool selected = (idx == s_cursor);

        // Row highlight
        bool isMarkedRow = !s_atRoot && s_marked[idx];
        if (selected)
            FillRect(0.f, ry, SW, ry + ROW_H, D3DCOLOR_XRGB(20, 40, 100));
        else if (i & 1)
            FillRect(0.f, ry, SW, ry + ROW_H, D3DCOLOR_XRGB(10, 12, 28));

        FileEntry& e = s_entries[idx];

        // Directory / file icon char — show check mark if selected
        bool isMarked = !s_atRoot && s_marked[idx];
        DrawText(ICON_X, ry, isMarked ? "*" : (e.isDir ? ">" : " "), 1.2f,
            isMarked ? COL_CYAN : (e.isDir ? COL_YELLOW : COL_DIM));

        // Name (truncated)
        char dispName[MAX_NAME_LEN];
        TruncName(e.name, dispName, NAME_MAX_CHARS, sizeof(dispName));

        DWORD nameCol;
        if (isMarkedRow)      nameCol = D3DCOLOR_XRGB(80, 255, 100);
        else if (selected)    nameCol = COL_WHITE;
        else if (e.isDir)     nameCol = COL_YELLOW;
        else if (IsXBE(e.name)) nameCol = COL_CYAN;
        else                  nameCol = D3DCOLOR_XRGB(180, 180, 180);

        DrawText(NAME_X, ry, dispName, 1.2f, nameCol);

        // Size (files only)
        if (!e.isDir)
        {
            char szBuf[16];
            FormatSize(e.sizeLow, szBuf, sizeof(szBuf));
            DrawTextR(SIZE_X, ry, szBuf, 1.1f,
                selected ? COL_WHITE : COL_GRAY);
        }
        else
        {
            DrawTextR(SIZE_X, ry, "<DIR>", 1.1f,
                selected ? COL_YELLOW : D3DCOLOR_XRGB(80, 80, 80));
        }
    }

    // Scroll indicator (right edge)
    if (s_entryCount > ROWS_VISIBLE)
    {
        float listH = (float)ROWS_VISIBLE * ROW_H;
        float thumbH = listH * ((float)ROWS_VISIBLE / (float)s_entryCount);
        if (thumbH < 12.f) thumbH = 12.f;
        float thumbY = LIST_Y + listH * ((float)s_scroll / (float)s_entryCount);
        FillRect(SW - 6.f, LIST_Y, SW - 2.f, LIST_Y + listH,
            D3DCOLOR_XRGB(20, 25, 55));
        FillRect(SW - 6.f, thumbY, SW - 2.f, thumbY + thumbH,
            D3DCOLOR_XRGB(80, 120, 200));
    }

    // Empty dir message
    if (s_entryCount == 0)
        DrawText(LM, LIST_Y + ROW_H, "No files found.", 1.2f, COL_DIM);

    // ---- Controls legend (top-left, idle only) --------------------------
    if (s_fosState == FOS_IDLE && !s_opRunning)
    {
        const float CX = SW - LM - 148.f;
        const float CY = CONTENT_Y + 2.f;
        const float CPad = 5.f;
        const float CLH = 12.f;

        // Choose lines based on state
        const char* lines[8];
        int         nLines = 0;

        if (s_markedCount > 0)
        {
            lines[nLines++] = "[Y]     Mark/Unmark";
            lines[nLines++] = "[Black] Copy marked";
            lines[nLines++] = "[White] Cut marked";
            lines[nLines++] = "[B]     Delete marked";
            lines[nLines++] = "[Back]  Clear marks";
        }
        else if (s_clipCount > 0)
        {
            lines[nLines++] = "[Y]     Mark/Unmark";
            lines[nLines++] = "[Black] Paste (copy)";
            lines[nLines++] = "[White] Paste (move)";
            lines[nLines++] = "[Back]  Clear clipboard";
            lines[nLines++] = "[A]     Open / Enter";
            lines[nLines++] = "[B]     Back / Up";
        }
        else
        {
            lines[nLines++] = "[A]     Open / Enter";
            lines[nLines++] = "[B]     Back / Up";
            lines[nLines++] = "[X]     Launch XBE";
            lines[nLines++] = "[Y]     Mark item";
            lines[nLines++] = "[LT/RT] Page up/down";
            lines[nLines++] = "[Start] FTP on/off";
        }

        float cw = 148.f;
        float ch = CPad * 2.f + (float)nLines * CLH;

        FillRect(CX, CY, CX + cw, CY + ch,
            D3DCOLOR_ARGB(180, 6, 10, 24));
        HLine(CY, CX, CX + cw, COL_BORDER);
        HLine(CY + ch, CX, CX + cw, COL_BORDER);
        VLine(CX, CY, CY + ch, COL_BORDER);
        VLine(CX + cw, CY, CY + ch, COL_BORDER);

        float ty = CY + CPad;
        for (int li = 0; li < nLines; ++li)
        {
            DrawText(CX + CPad, ty, lines[li], 0.95f,
                D3DCOLOR_XRGB(160, 180, 210));
            ty += CLH;
        }
    }

    // ---- FTP widget (lower right) ----------------------------------------
    DrawFtpWidget();

    // ---- File op progress widget -----------------------------------------
    if (s_opRunning)
    {
        const float OW = 260.f;
        const float OX = (SW - OW) * 0.5f;
        const float OY = 180.f;
        const float OP = 8.f;
        const float OLH = 14.f;
        const float OH = OP * 2.f + OLH * 4.f;

        FillRect(OX, OY, OX + OW, OY + OH, D3DCOLOR_ARGB(230, 8, 12, 30));
        HLine(OY, OX, OX + OW, COL_CYAN);
        HLine(OY + OH, OX, OX + OW, COL_CYAN);

        float ty = OY + OP;
        DrawText(OX + OP, ty, "FILE OPERATION", 1.1f, COL_CYAN); ty += OLH;

        char itemLine[48];
        itemLine[0] = '\0';
        char a[8], b[8];
        IntToStr(s_opItemDone, a, sizeof(a));
        IntToStr(s_opItemTotal, b, sizeof(b));
        StrCat2(itemLine, sizeof(itemLine), itemLine, "Item ");
        StrCat2(itemLine, sizeof(itemLine), itemLine, a);
        StrCat2(itemLine, sizeof(itemLine), itemLine, " of ");
        StrCat2(itemLine, sizeof(itemLine), itemLine, b);
        DrawText(OX + OP, ty, itemLine, 1.05f, COL_WHITE); ty += OLH;

        DrawText(OX + OP, ty, s_opSrcName, 1.05f, D3DCOLOR_XRGB(180, 180, 180)); ty += OLH;

        // Progress bar for current file
        float frac = (s_opTotal > 0) ? (float)s_opDone / (float)s_opTotal : 0.f;
        if (frac > 1.f) frac = 1.f;
        const float BW = OW - OP * 2.f - 36.f;
        FillRect(OX + OP, ty + 1.f, OX + OP + BW, ty + 9.f, D3DCOLOR_XRGB(15, 20, 50));
        FillRectGrad(OX + OP, ty + 1.f, OX + OP + BW * frac, ty + 9.f,
            D3DCOLOR_XRGB(60, 180, 255), D3DCOLOR_XRGB(20, 80, 160));
        HLine(ty + 1.f, OX + OP, OX + OP + BW, COL_BORDER);
        HLine(ty + 9.f, OX + OP, OX + OP + BW, COL_BORDER);
        char pctBuf[8];
        IntToStr(Ftoi(frac * 100.f), pctBuf, sizeof(pctBuf));
        AppendStr(pctBuf, sizeof(pctBuf), "%");
        DrawText(OX + OP + BW + 4.f, ty, pctBuf, 1.05f, COL_WHITE);
    }

    // ---- Confirm delete overlay ------------------------------------------
    if (s_fosState == FOS_CONFIRM_DELETE)
    {
        const float DW = 240.f;
        const float DH = 70.f;
        const float DX = (SW - DW) * 0.5f;
        const float DY = 200.f;

        FillRect(DX, DY, DX + DW, DY + DH, D3DCOLOR_ARGB(240, 30, 8, 8));
        HLine(DY, DX, DX + DW, D3DCOLOR_XRGB(200, 50, 50));
        HLine(DY + DH, DX, DX + DW, D3DCOLOR_XRGB(200, 50, 50));

        char delLine[48] = "Delete ";
        char cnt[8];
        IntToStr(s_markedCount, cnt, sizeof(cnt));
        AppendStr(delLine, sizeof(delLine), cnt);
        AppendStr(delLine, sizeof(delLine), " item(s)?");
        DrawText(DX + 10.f, DY + 12.f, delLine, 1.15f, COL_WHITE);
        DrawText(DX + 10.f, DY + 30.f, "[B] Confirm", 1.05f, D3DCOLOR_XRGB(255, 80, 80));
        DrawText(DX + 10.f, DY + 46.f, "[Back] Cancel", 1.05f, COL_GRAY);
    }

    // ---- Clipboard status bar (bottom of list, above bot bar) ------------
    if (s_clipCount > 0 && !s_opRunning && s_fosState == FOS_IDLE)
    {
        char clipLine[64] = "Clipboard: ";
        char cnt[8];
        IntToStr(s_clipCount, cnt, sizeof(cnt));
        AppendStr(clipLine, sizeof(clipLine), cnt);
        AppendStr(clipLine, sizeof(clipLine), " item(s)  ");
        AppendStr(clipLine, sizeof(clipLine), s_clipOp == FILEOP_MOVE ? "[MOVE]" : "[COPY]");
        DrawText(LM, BOT_BAR_Y - 16.f, clipLine, 1.05f, COL_CYAN);
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// FileExplorer_Tick
// ============================================================================

void FileExplorer_Tick(const DiagLogo& logo)
{
    // Service FTP and file ops every tick regardless of input skip
    FtpTick();
    FileOpTick();

    if (s_skipFirstTick)
    {
        s_prevBtns = GetButtons();
        s_skipFirstTick = false;
        Render(logo);
        return;
    }

    WORD cur = GetButtons();
    int lt = 0, rt = 0, blk = 0, wht = 0, btnA = 0, btnB = 0, btnX = 0, btnY = 0;
    GetTriggers(lt, rt, blk, wht, btnA, btnB, btnX, btnY);
    (void)lt; (void)rt; (void)blk; (void)wht;
    (void)btnA; (void)btnB; (void)btnX; (void)btnY;

    // [Start] toggle FTP
    if (EdgeDown(cur, s_prevBtns, BTN_START))
    {
        if (s_ftp.state == FTP_OFF)
        {
            // Check physical link first (same check PrometheOS network.cpp uses)
            DWORD linkStatus = XNetGetEthernetLinkStatus();
            if (!(linkStatus & XNET_ETHERNET_LINK_ACTIVE))
            {
                StrCopy(s_ftpMsg, sizeof(s_ftpMsg), "No Ethernet link");
                s_ftpMsgFrames = 120; // ~4s at 30fps
            }
            else if (!s_ipOK)
            {
                // Link present but no IP yet — re-poll once
                ResolveIP();
                if (s_ipOK)
                    FtpStart();
                else
                {
                    StrCopy(s_ftpMsg, sizeof(s_ftpMsg), "No IP address");
                    s_ftpMsgFrames = 120;
                }
            }
            else
            {
                FtpStart();
            }
        }
        else
            FtpStop();
    }

    // Block navigation input while file op running
    if (s_opRunning) { s_prevBtns = cur; Render(logo); return; }

    // [DPad Down]
    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_DOWN))
    {
        if (s_cursor < s_entryCount - 1)
        {
            s_cursor++;
            if (s_cursor >= s_scroll + ROWS_VISIBLE)
                s_scroll = s_cursor - ROWS_VISIBLE + 1;
        }
    }

    // [DPad Up]
    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_UP))
    {
        if (s_cursor > 0)
        {
            s_cursor--;
            if (s_cursor < s_scroll)
                s_scroll = s_cursor;
        }
    }

    // [LT] page up
    if (EdgeDown(cur, s_prevBtns, BTN_LTRIG))
    {
        s_cursor -= ROWS_VISIBLE;
        if (s_cursor < 0) s_cursor = 0;
        s_scroll = s_cursor;
        if (s_scroll > s_cursor - ROWS_VISIBLE + 1)
            s_scroll = s_cursor - ROWS_VISIBLE + 1;
        if (s_scroll < 0) s_scroll = 0;
    }

    // [RT] page down
    if (EdgeDown(cur, s_prevBtns, BTN_RTRIG))
    {
        s_cursor += ROWS_VISIBLE;
        if (s_cursor >= s_entryCount) s_cursor = s_entryCount - 1;
        s_scroll = s_cursor - ROWS_VISIBLE + 1;
        if (s_scroll < 0) s_scroll = 0;
    }

    // [A] enter / open
    if (EdgeDown(cur, s_prevBtns, BTN_A))
    {
        if (s_entryCount > 0)
        {
            FileEntry& e = s_entries[s_cursor];
            if (e.isDir || s_atRoot)
                EnterSelected();
            // [A] on a file does nothing — use [X] to launch XBEs
        }
    }

    // [Y] mark / unmark current item
    if (EdgeDown(cur, s_prevBtns, BTN_Y))
    {
        if (!s_atRoot && s_entryCount > 0 && s_fosState == FOS_IDLE && !s_opRunning)
        {
            s_marked[s_cursor] = !s_marked[s_cursor];
            s_markedCount += s_marked[s_cursor] ? 1 : -1;
            if (s_markedCount < 0) s_markedCount = 0;
            // Advance cursor
            if (s_cursor < s_entryCount - 1)
            {
                s_cursor++;
                if (s_cursor >= s_scroll + ROWS_VISIBLE)
                    s_scroll = s_cursor - ROWS_VISIBLE + 1;
            }
        }
    }

    // [Black] — copy marked to clipboard, or paste clipboard here
    if (EdgeDown(cur, s_prevBtns, BTN_BLACK))
    {
        if (s_fosState == FOS_IDLE && !s_atRoot)
        {
            if (s_markedCount > 0)
            {
                // Snapshot marked items as COPY clipboard
                SnapMarkedToClipboard(FILEOP_COPY);
            }
            else if (s_clipCount > 0)
            {
                // Paste clipboard into current directory
                FileOpStart(s_clipOp == FILEOP_NONE ? FILEOP_COPY : s_clipOp);
            }
        }
    }

    // [White] — move marked items here, or move clipboard here
    if (EdgeDown(cur, s_prevBtns, BTN_WHITE))
    {
        if (s_fosState == FOS_IDLE && !s_atRoot)
        {
            if (s_markedCount > 0)
            {
                // Snapshot as MOVE clipboard — user navigates to dest, then
                // presses White (or Black) again to execute
                SnapMarkedToClipboard(FILEOP_MOVE);
            }
            else if (s_clipCount > 0)
            {
                FileOpStart(FILEOP_MOVE);
            }
        }
    }

    // [B] — confirm delete if pending, else go up / back to menu
    if (EdgeDown(cur, s_prevBtns, BTN_B))
    {
        if (s_fosState == FOS_CONFIRM_DELETE)
        {
            // Confirmed — delete all marked items
            s_fosState = FOS_IDLE;
            for (int i = 0; i < s_entryCount; ++i)
            {
                if (!s_marked[i]) continue;
                char fullPath[MAX_PATH_LEN];
                StrCopy(fullPath, sizeof(fullPath), s_path);
                int pl = 0; while (fullPath[pl]) pl++;
                if (pl > 0 && fullPath[pl - 1] != '\\') { fullPath[pl++] = '\\'; fullPath[pl] = '\0'; }
                AppendStr(fullPath, sizeof(fullPath), s_entries[i].name);
                FileDeleteRecursive(fullPath, s_entries[i].isDir);
            }
            for (int i = 0; i < MAX_ENTRIES; ++i) s_marked[i] = false;
            s_markedCount = 0;
            LoadDirectory(s_path);
            s_cursor = 0; s_scroll = 0;
        }
        else if (s_fosState == FOS_IDLE)
        {
            if (s_markedCount > 0)
            {
                // First [B] with marks — show confirm prompt
                s_fosState = FOS_CONFIRM_DELETE;
            }
            else
            {
                GoUp();
            }
        }
    }

    // [Back] button — cancel confirm or clear marks
    if (EdgeDown(cur, s_prevBtns, BTN_BACK))
    {
        if (s_fosState == FOS_CONFIRM_DELETE)
        {
            s_fosState = FOS_IDLE;
        }
        else
        {
            // Clear all marks and clipboard
            for (int i = 0; i < MAX_ENTRIES; ++i) s_marked[i] = false;
            s_markedCount = 0;
            s_clipCount = 0;
            s_clipOp = FILEOP_NONE;
        }
    }

    // [X] launch XBE (only when nothing marked)
    if (EdgeDown(cur, s_prevBtns, BTN_X))
    {
        if (!s_atRoot && s_entryCount > 0 && s_markedCount == 0)
        {
            FileEntry& e = s_entries[s_cursor];
            if (e.isDir)
            {
                // directories silently ignore — hint bar already hides [X] for dirs
            }
            else if (!IsXBE(e.name))
            {
                StrCopy(s_ftpMsg, sizeof(s_ftpMsg), "Not an XBE file");
                s_ftpMsgFrames = 90;
            }
            else
            {
                // XLaunchNewImage only reliably works from D: on RXDK.
                // Remap D: to the directory containing the XBE, then launch
                // as "D:\filename.xbe" — same pattern as PrometheOS.
                //
                // IoCreateSymbolicLink needs a \Device\... path, not a dos path.
                // Map known drive letters to their partition device paths, then
                // append the subdirectory portion of s_path.

                // Partition map: matches MountAllDrives() table
                struct { char letter; const char* device; } partMap[] = {
                    { 'C', "\\Device\\Harddisk0\\Partition2" },
                    { 'E', "\\Device\\Harddisk0\\Partition1" },
                    { 'F', "\\Device\\Harddisk0\\Partition6" },
                    { 'G', "\\Device\\Harddisk0\\Partition7" },
                    { 'X', "\\Device\\Harddisk0\\Partition3" },
                    { 'Y', "\\Device\\Harddisk0\\Partition4" },
                    { 'Z', "\\Device\\Harddisk0\\Partition5" },
                    { 'D', "\\Device\\CdRom0"                },
                    { 0,   NULL }
                };

                // s_path is e.g. "C:\TDATA\mygame" or "C:\"
                char driveLetter = (s_path[0] >= 'a') ? s_path[0] - 32 : s_path[0];
                const char* partDevice = NULL;
                for (int pi = 0; partMap[pi].letter; ++pi)
                    if (partMap[pi].letter == driveLetter)
                    {
                        partDevice = partMap[pi].device; break;
                    }

                if (partDevice == NULL)
                {
                    // Unknown drive — can't remap, abort
                    StrCopy(s_ftpMsg, sizeof(s_ftpMsg), "Unknown drive letter");
                    s_ftpMsgFrames = 90;
                }
                else
                {
                    // Build device path: partition + subdirectory (if any)
                    // s_path+2 skips "C:" leaving "\subdir\..." or "\"
                    char devPath[MAX_PATH_LEN + 40];
                    StrCopy(devPath, sizeof(devPath), partDevice);
                    const char* subdir = s_path + 2; // skip "C:"
                    // subdir is "\foo\bar" or "\" — append without trailing slash
                    int subLen = 0; while (subdir[subLen]) subLen++;
                    // Strip trailing backslash from subdir portion
                    while (subLen > 1 && subdir[subLen - 1] == '\\') subLen--;
                    // Only append if there's a real subdir (not just "\")
                    if (subLen > 1)
                    {
                        int devLen = 0; while (devPath[devLen]) devLen++;
                        for (int si = 0; si < subLen && devLen < (int)sizeof(devPath) - 1; si++)
                            devPath[devLen++] = subdir[si];
                        devPath[devLen] = '\0';
                    }

                    int devLen = 0; while (devPath[devLen]) devLen++;
                    char linkBuf[] = "\\??\\D:";
                    XBOX_STRING sLink = { 6, 7, linkBuf };
                    XBOX_STRING sDev = { (USHORT)devLen, (USHORT)(devLen + 1), devPath };

                    LONG r = 0;
                    r |= IoDeleteSymbolicLink(&sLink);
                    r |= IoCreateSymbolicLink(&sLink, &sDev);

                    if (r != 0)
                    {
                        StrCopy(s_ftpMsg, sizeof(s_ftpMsg), "Remap D: failed");
                        s_ftpMsgFrames = 90;
                    }
                    else
                    {
                        char launchPath[MAX_NAME_LEN + 4];
                        launchPath[0] = 'D'; launchPath[1] = ':'; launchPath[2] = '\\'; launchPath[3] = '\0';
                        AppendStr(launchPath, sizeof(launchPath), e.name);

                        FtpStop();
                        XLaunchNewImage(launchPath, NULL);
                        // Never returns
                    }
                }
            }
        }
    }

    s_prevBtns = cur;
    Render(logo);
}
