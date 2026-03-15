// FileExplorer.cpp
// XbDiag - Single-pane file explorer.
//
// ===========================================================================
// Navigation
// ===========================================================================
//
//  [A]           Enter directory / (at drive list) open drive
//  [B]           Go up one level; with marks = delete confirm; at root = back to menu
//  [Back]        Cancel delete confirm / clear all marks and clipboard
//  [X]           Launch selected XBE via XLaunchNewImage (one-way, no return)
//  [Y]           Mark / unmark current item (advances cursor after mark)
//  [Black]       Copy marked items to picker destination (or paste clipboard here)
//  [White]       Move marked items to picker destination (or paste clipboard here)
//  [DPad Up/Dn]  Move cursor
//  [LT / RT]     Page up / page down
//  [Start]       Toggle FTP server on / off
//  [Back+Black]  Format MU (at drive root, cursor on MU entry)
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
//                DELE MKD RMD RNFR RNTO SIZE NOOP FEAT AUTH PBSZ PROT
//                MODE REST ALLO STAT EPSV MDTM
//  All sockets : non-blocking (FIONBIO) — polled every tick, never stalls
//
//  State machine (g_ftp.*):
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
#include "FtpServ.h"
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

// MU mount API — matches PrometheOS inputManager.cpp pattern
// MU_CreateDeviceObject opens the MU device and returns its device name via deviceName.
// IoCreateSymbolicLink then maps that device name to a drive letter (e.g. "\\??\\A:").
// Letter mapping: A=P0 top  B=P0 bot  C=P1 top  D=P1 bot  E=P2 top  F=P2 bot  G=P3 top  H=P3 bot
extern "C"
{
    LONG  WINAPI MU_CreateDeviceObject(DWORD port, DWORD slot, XBOX_STRING* deviceName);
    VOID  WINAPI MU_CloseDeviceObject(DWORD port, DWORD slot);
    // IoDismountVolume — unmounts a live volume before format (PrometheOS pattern)
    LONG  WINAPI IoDismountVolume(void* deviceObject);
    VOID* WINAPI MU_GetExistingDeviceObject(DWORD port, DWORD slot);
    // XapiFormatFATVolumeEx — formats a volume as FATX given a device path and cluster size
    // clusterSize = 0 lets the kernel pick the correct size for a MU
    BOOL  WINAPI XapiFormatFATVolumeEx(XBOX_STRING* devicePath, DWORD clusterSize);
}
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

// --- MU hotplug tracking — reload drive list if insertion/removal detected ---
static bool      s_muSnapshot[4][2];  // last-known MU present state

// --- MU format confirm overlay ---
static bool      s_muFormatPending = false;  // Back+Black pressed on a MU entry
static int       s_muFormatPort = -1;
static int       s_muFormatSlot = -1;
static char      s_muFormatLabel[16];        // e.g. "P1 MMU1"

// --- File operation state ---
enum FileOpType { FILEOP_NONE = 0, FILEOP_COPY, FILEOP_MOVE };
enum FileOpState { FOS_IDLE = 0, FOS_CONFIRM_DELETE, FOS_EXPANDING, FOS_RUNNING, FOS_PICK_DEST };

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

// Destination picker state (FOS_PICK_DEST)
static FileEntry      s_pickEntries[MAX_ENTRIES];
static int            s_pickEntryCount = 0;
static int            s_pickCursor = 0;
static int            s_pickScroll = 0;
static char           s_pickPath[MAX_PATH_LEN];
static bool           s_pickAtRoot = true;
static FileOpType     s_pendingOp = FILEOP_NONE;
static int            s_expandIdx = 0;                // clipboard index during FOS_EXPANDING
static char           s_expandDstRoot[MAX_PATH_LEN];  // dest root used during expansion

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
static bool      s_workTruncated = false;  // true if work list hit MAX_WORK_ITEMS
static int       s_opSkipCount = 0;       // files skipped due to open/write failure
static int       s_opDelFail = 0;       // delete failures during MOVE cleanup
static bool      s_opCopyOK = true;    // false if any file copy write failed

// --- FTP ---


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

// Mount any inserted Memory Units and return a bitmask of which letters got mounted.
// Letters A-H: bit 0 = A (P0 top), bit 1 = B (P0 bottom), ..., bit 7 = H (P3 bottom).
static BYTE MountMUs()
{
    BYTE mounted = 0;
    for (int port = 0; port < 4; ++port)
    {
        for (int slot = 0; slot < 2; ++slot)
        {
            if (!IsMUPresent(port, slot)) continue;

            char driveLetter = 'A' + (char)(port * 2 + slot);

            // Build the device name via MU_CreateDeviceObject (mirrors PrometheOS)
            char devBuf[64];
            XBOX_STRING devName;
            devName.Length = 0;
            devName.MaximumLength = sizeof(devBuf) - 2;
            devName.Buffer = devBuf;

            if (MU_CreateDeviceObject((DWORD)port, (DWORD)slot, &devName) < 0)
                continue;  // MU not readable — skip

            // Map device to drive letter: "\\??\\ X:"
            char linkBuf[8];
            linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
            linkBuf[4] = driveLetter; linkBuf[5] = ':'; linkBuf[6] = '\0';

            XBOX_STRING sLink = { 6, 7, linkBuf };
            IoCreateSymbolicLink(&sLink, &devName);
            // Collision (already mounted) is fine — ignore return value

            mounted |= (1 << (port * 2 + slot));
        }
    }
    return mounted;
}

// Format a Memory Unit as FATX. Dismounts the volume, formats, remounts.
// Returns true on success.
static bool FormatMU(int port, int slot)
{
    // Get the device object for dismount
    void* devObj = MU_GetExistingDeviceObject((DWORD)port, (DWORD)slot);
    if (devObj)
        IoDismountVolume(devObj);

    // Close the existing device object so we can reopen it for format
    MU_CloseDeviceObject((DWORD)port, (DWORD)slot);

    // Reopen to get a fresh device path
    char devBuf[64];
    XBOX_STRING devName;
    devName.Length = 0;
    devName.MaximumLength = sizeof(devBuf) - 2;
    devName.Buffer = devBuf;

    if (MU_CreateDeviceObject((DWORD)port, (DWORD)slot, &devName) < 0)
        return false;

    // Format as FATX — cluster size 0 = kernel chooses appropriate size for MU
    BOOL ok = XapiFormatFATVolumeEx(&devName, 0);

    // Remap the drive letter
    char driveLetter = 'A' + (char)(port * 2 + slot);
    char linkBuf[8];
    linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
    linkBuf[4] = driveLetter; linkBuf[5] = ':'; linkBuf[6] = '\0';
    XBOX_STRING sLink = { 6, 7, linkBuf };
    IoCreateSymbolicLink(&sLink, &devName);

    return ok != FALSE;
}

// ============================================================================
static void LoadDriveList()
{
    s_entryCount = 0;
    s_atRoot = true;
    s_path[0] = '\0';
    for (int i = 0; i < MAX_ENTRIES; ++i) s_marked[i] = false;
    s_markedCount = 0;

    // Ensure HDD partitions and any inserted MUs are mapped to drive letters
    MountAllDrives();
    MountMUs();

    // HDD drives — always probed
    const char* hddDrives[] = { "C", "D", "E", "F", "G", "X", "Y", "Z" };
    for (int d = 0; d < 8 && s_entryCount < MAX_ENTRIES; ++d)
    {
        char pattern[8];
        pattern[0] = hddDrives[d][0]; pattern[1] = ':'; pattern[2] = '\\';
        pattern[3] = '*'; pattern[4] = '\0';

        WIN32_FIND_DATA fd;
        HANDLE h = FindFirstFile(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            FindClose(h);
            FileEntry& e = s_entries[s_entryCount++];
            e.name[0] = hddDrives[d][0]; e.name[1] = ':'; e.name[2] = '\0';
            e.isDir = true;
            e.sizeLow = 0;
        }
    }

    // Memory Units — probe A-H, show only those that are mounted and accessible
    // MMU1 = top slot, MMU2 = bottom slot. Drive letter stored in sizeLow for open.
    static const char* k_muLabels[8] = {
        "P1 MMU1", "P1 MMU2",
        "P2 MMU1", "P2 MMU2",
        "P3 MMU1", "P3 MMU2",
        "P4 MMU1", "P4 MMU2",
    };
    for (int mu = 0; mu < 8 && s_entryCount < MAX_ENTRIES; ++mu)
    {
        int port = mu / 2, slot = mu % 2;
        if (!IsMUPresent(port, slot)) continue;
        char muPat[8];
        muPat[0] = 'A' + (char)mu; muPat[1] = ':'; muPat[2] = '\\';
        muPat[3] = '\0';
        // Use GetFileAttributesA on the root — succeeds even on empty MUs.
        // FindFirstFile("X:\\*") fails on empty volumes, so don't use it here.
        DWORD attr = GetFileAttributesA(muPat);
        if (attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            FileEntry& e = s_entries[s_entryCount++];
            StrCopy(e.name, sizeof(e.name), k_muLabels[mu]);
            e.isDir = true;
            e.sizeLow = (DWORD)('A' + mu);  // drive letter stored for open
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

// ============================================================================
// Destination picker — separate buffer, mirrors main browser logic
// ============================================================================

static void PickLoadDriveList()
{
    s_pickEntryCount = 0;
    s_pickAtRoot = true;
    s_pickPath[0] = '\0';

    const char* drives[] = { "C", "D", "E", "F", "G", "X", "Y", "Z" };
    for (int d = 0; d < 8 && s_pickEntryCount < MAX_ENTRIES; ++d)
    {
        char pattern[8];
        pattern[0] = drives[d][0]; pattern[1] = ':'; pattern[2] = '\\';
        pattern[3] = '*'; pattern[4] = '\0';

        WIN32_FIND_DATA fd;
        HANDLE h = FindFirstFile(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            FindClose(h);
            FileEntry& e = s_pickEntries[s_pickEntryCount++];
            e.name[0] = drives[d][0]; e.name[1] = ':'; e.name[2] = '\0';
            e.isDir = true;
            e.sizeLow = 0;
        }
    }
}

static void PickLoadDirectory(const char* path)
{
    s_pickEntryCount = 0;
    s_pickAtRoot = false;
    StrCopy(s_pickPath, sizeof(s_pickPath), path);

    char pattern[MAX_PATH_LEN + 4];
    StrCopy(pattern, sizeof(pattern), path);
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
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' || (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;

        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue; // dirs only

        if (s_pickEntryCount >= MAX_ENTRIES) break;

        FileEntry& e = s_pickEntries[s_pickEntryCount++];
        TruncName(fd.cFileName, e.name, MAX_NAME_LEN - 1, MAX_NAME_LEN);
        e.isDir = true;
        e.sizeLow = 0;
    } while (FindNextFile(h, &fd));

    FindClose(h);

    // Sort alphabetically
    for (int i = 0; i < s_pickEntryCount - 1; ++i)
        for (int j = i + 1; j < s_pickEntryCount; ++j)
        {
            const char* a = s_pickEntries[i].name;
            const char* b = s_pickEntries[j].name;
            int k = 0;
            while (a[k] && b[k] && a[k] == b[k]) k++;
            if (a[k] > b[k])
            {
                FileEntry tmp = s_pickEntries[i];
                s_pickEntries[i] = s_pickEntries[j];
                s_pickEntries[j] = tmp;
            }
        }
}

// ============================================================================
// DrawDestPicker — modal overlay card, drawn on top of everything
// ============================================================================

#define PICK_ROWS_VISIBLE  12
#define PICK_ROW_H         (LINE_H + 1.f)

static void DrawDestPicker()
{
    const float PW = 380.f;
    const float PH = 290.f;
    const float PX = (SW - PW) * 0.5f;
    const float PY = (480.f - PH) * 0.5f;

    // Backdrop
    FillRectGrad(PX, PY, PX + PW, PY + PH,
        D3DCOLOR_XRGB(14, 20, 52),
        D3DCOLOR_XRGB(8, 12, 32));
    HLine(PY, PX, PX + PW, COL_CYAN);
    HLine(PY + PH, PX, PX + PW, COL_CYAN);
    VLine(PX, PY, PY + PH, COL_BORDER);
    VLine(PX + PW, PY, PY + PH, COL_BORDER);

    // Title
    const char* opLabel = (s_pendingOp == FILEOP_MOVE) ? "MOVE TO" : "COPY TO";
    DrawText(PX + 8.f, PY + 5.f, opLabel, 1.3f, COL_YELLOW);

    // Current path bar
    const char* pathDisp = s_pickAtRoot ? "[ Drive List ]" : s_pickPath;
    DrawText(PX + 8.f, PY + 22.f, pathDisp, 1.05f, COL_CYAN);
    HLine(PY + 34.f, PX, PX + PW, COL_BORDER);

    // Entry list
    const float LIST_TOP = PY + 37.f;
    const float ICON_X = PX + 8.f;
    const float NAME_X = PX + 22.f;

    for (int i = 0; i < PICK_ROWS_VISIBLE; ++i)
    {
        int idx = s_pickScroll + i;
        if (idx >= s_pickEntryCount) break;

        float ry = LIST_TOP + (float)i * PICK_ROW_H;
        bool sel = (idx == s_pickCursor);

        if (sel)
            FillRect(PX + 1.f, ry, PX + PW - 1.f, ry + PICK_ROW_H,
                D3DCOLOR_XRGB(20, 40, 100));
        else if (i & 1)
            FillRect(PX + 1.f, ry, PX + PW - 1.f, ry + PICK_ROW_H,
                D3DCOLOR_XRGB(10, 12, 28));

        FileEntry& e = s_pickEntries[idx];
        DWORD nc = sel ? COL_WHITE : COL_YELLOW;
        DrawText(ICON_X, ry, ">", 1.2f, sel ? COL_CYAN : COL_DIM);
        DrawText(NAME_X, ry, e.name, 1.2f, nc);
    }

    // Empty dir message
    if (s_pickEntryCount == 0)
    {
        const char* msg = s_pickAtRoot ? "No drives found" : "No subfolders";
        DrawText(PX + (PW - TW(msg, 1.1f)) * 0.5f,
            LIST_TOP + 20.f, msg, 1.1f, COL_DIM);
    }

    // Scroll indicator
    if (s_pickEntryCount > PICK_ROWS_VISIBLE)
    {
        float sbX = PX + PW - 6.f;
        float sbY0 = LIST_TOP;
        float sbH = (float)PICK_ROWS_VISIBLE * PICK_ROW_H;
        float thH = sbH * ((float)PICK_ROWS_VISIBLE / (float)s_pickEntryCount);
        float thY = sbY0 + sbH * ((float)s_pickScroll / (float)s_pickEntryCount);
        FillRect(sbX, sbY0, sbX + 4.f, sbY0 + sbH, D3DCOLOR_XRGB(20, 25, 55));
        FillRect(sbX, thY, sbX + 4.f, thY + thH, COL_BORDER);
    }

    // Hint bar inside card
    HLine(PY + PH - 18.f, PX, PX + PW, COL_BORDER);
    const char* hint = s_pickAtRoot
        ? "[A] Open Drive    [B] Cancel"
        : "[A] Enter Folder  [Black/White] Copy/Move Here  [B] Up";
    DrawText(PX + (PW - TW(hint, 1.0f)) * 0.5f,
        PY + PH - 15.f, hint, 1.0f, COL_GRAY);
}

// ============================================================================
// Navigate into entry at cursor
static void EnterSelected()
{
    if (s_entryCount == 0) return;
    FileEntry& e = s_entries[s_cursor];

    if (s_atRoot)
    {
        // HDD drives: letter is name[0].  MU entries: letter stored in sizeLow.
        char driveLetter = (e.sizeLow >= (DWORD)'A' && e.sizeLow <= (DWORD)'H')
            ? (char)e.sizeLow
            : e.name[0];
        char drivePath[8];
        drivePath[0] = driveLetter; drivePath[1] = ':';
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

// Recursively expand srcPath into s_work[], building a flat list of
// WI_MKDIR and WI_FILE entries.
static void ExpandToWorkList(const char* srcPath, const char* dstPath, bool isDir)
{
    if (s_workCount >= MAX_WORK_ITEMS) { s_workTruncated = true; return; }

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
        if (s_workCount >= MAX_WORK_ITEMS) { s_workTruncated = true; break; }

        int srcBase = 0; while (srcPath[srcBase]) srcBase++;
        int nameLen = 0; while (fd.cFileName[nameLen]) nameLen++;
        if (srcBase + 1 + nameLen + 1 > MAX_PATH_LEN) continue;
        int dstBase = 0; while (dstPath[dstBase]) dstBase++;
        if (dstBase + 1 + nameLen + 1 > MAX_PATH_LEN) continue;

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

static bool FileDeleteRecursive(const char* path, bool isDir)
{
    if (!isDir)
    {
        bool ok = DeleteFileA(path) != 0;
        if (!ok) s_opDelFail++;
        return ok;
    }

    char pat[MAX_PATH_LEN + 4];
    StrCopy(pat, sizeof(pat), path);
    int pl = 0; while (pat[pl]) pl++;
    if (pl > 0 && pat[pl - 1] != '\\') { pat[pl++] = '\\'; pat[pl] = '\0'; }
    pat[pl++] = '*'; pat[pl] = '\0';

    bool allOK = true;
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pat, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            if (fd.cFileName[0] == '.' &&
                (fd.cFileName[1] == '\0' || fd.cFileName[1] == '.')) continue;

            int pathLen = 0; while (path[pathLen]) pathLen++;
            int nameLen = 0; while (fd.cFileName[nameLen]) nameLen++;
            if (pathLen + 1 + nameLen + 1 > MAX_PATH_LEN)
            {
                allOK = false; s_opDelFail++;
                continue;
            }

            char child[MAX_PATH_LEN];
            StrCopy(child, sizeof(child), path);
            int cl = 0; while (child[cl]) cl++;
            if (cl > 0 && child[cl - 1] != '\\') { child[cl++] = '\\'; child[cl] = '\0'; }
            AppendStr(child, sizeof(child), fd.cFileName);
            if (!FileDeleteRecursive(child,
                (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0))
                allOK = false;
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryA(path)) { allOK = false; s_opDelFail++; }
    return allOK;
}

// Start a file operation — expand clipboard to flat work list, begin ticking.
static void FileOpStartTo(const char* destDir)
{
    s_workCount = 0;
    s_workIdx = 0;
    s_workOp = s_clipOp;
    s_opItemDone = 0;
    s_opSrcHandle = INVALID_HANDLE_VALUE;
    s_opDstHandle = INVALID_HANDLE_VALUE;
    s_workTruncated = false;
    s_opSkipCount = 0;
    s_opDelFail = 0;
    s_opCopyOK = true;

    StrCopy(s_expandDstRoot, sizeof(s_expandDstRoot), destDir);
    s_expandIdx = 0;
    s_opRunning = (s_clipCount > 0);
    s_fosState = s_opRunning ? FOS_EXPANDING : FOS_IDLE;
}

static void FileOpStart(FileOpType op)
{
    s_workCount = 0;
    s_workIdx = 0;
    s_workOp = op;
    s_opItemDone = 0;
    s_opSrcHandle = INVALID_HANDLE_VALUE;
    s_opDstHandle = INVALID_HANDLE_VALUE;
    s_workTruncated = false;
    s_opSkipCount = 0;
    s_opDelFail = 0;
    s_opCopyOK = true;

    StrCopy(s_expandDstRoot, sizeof(s_expandDstRoot), s_path);
    s_expandIdx = 0;
    s_opRunning = (s_clipCount > 0);
    s_fosState = s_opRunning ? FOS_EXPANDING : FOS_IDLE;
}

// Called every tick while s_opRunning. Processes one 64KB chunk per call.
static void FileOpTick()
{
    if (!s_opRunning) return;

    // FOS_EXPANDING: expand one clipboard entry per tick into the work list.
    // Spreads ExpandToWorkList (recursive FindFirstFile) across frames so
    // the render loop stays responsive on large or deep directories.
    if (s_fosState == FOS_EXPANDING)
    {
        if (s_expandIdx < s_clipCount)
        {
            ClipboardEntry& ce = s_clipboard[s_expandIdx];
            int srcLen = 0; while (ce.path[srcLen]) srcLen++;
            int lastSep = -1;
            for (int k = srcLen - 1; k >= 0; --k)
                if (ce.path[k] == '\\') { lastSep = k; break; }
            const char* fname = (lastSep >= 0) ? ce.path + lastSep + 1 : ce.path;
            char dst[MAX_PATH_LEN];
            StrCopy(dst, sizeof(dst), s_expandDstRoot);
            int dl = 0; while (dst[dl]) dl++;
            if (dl > 0 && dst[dl - 1] != '\\') { dst[dl++] = '\\'; dst[dl] = '\0'; }
            AppendStr(dst, sizeof(dst), fname);
            ExpandToWorkList(ce.path, dst, ce.isDir);
            ++s_expandIdx;
        }
        if (s_expandIdx >= s_clipCount)
        {
            s_opItemTotal = s_workCount;
            s_fosState = (s_workCount > 0) ? FOS_RUNNING : FOS_IDLE;
            if (s_fosState == FOS_IDLE) s_opRunning = false;
        }
        return;
    }

    // Time-bounded: stop mkdir loop after 20ms to avoid stalling a frame
    // on large directory trees.
    DWORD tickStart = GetTickCount();

    while (s_workIdx < s_workCount)
    {
        WorkItem& wi = s_work[s_workIdx];

        if (wi.type == WI_MKDIR)
        {
            CreateDirectoryA(wi.dst, NULL);
            s_opItemDone++;
            s_workIdx++;
            if (GetTickCount() - tickStart >= 20) break;
            continue;
        }

        // WI_FILE — open handles on first access
        if (s_opSrcHandle == INVALID_HANDLE_VALUE)
        {
            s_opSrcHandle = CreateFile(wi.src, GENERIC_READ, FILE_SHARE_READ,
                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (s_opSrcHandle == INVALID_HANDLE_VALUE)
            {
                s_opSkipCount++; s_opCopyOK = false;
                s_opItemDone++; s_workIdx++; continue;
            }

            s_opDstHandle = CreateFile(wi.dst, GENERIC_WRITE, 0,
                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (s_opDstHandle == INVALID_HANDLE_VALUE)
            {
                CloseHandle(s_opSrcHandle);
                s_opSrcHandle = INVALID_HANDLE_VALUE;
                s_opSkipCount++; s_opCopyOK = false;
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
        if (!WriteFile(s_opDstHandle, s_copyBuf, nr, &nw, NULL) || nw != nr)
            s_opCopyOK = false;
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

        // MOVE: delete sources — only if all copies completed cleanly.
        // If any file was skipped or a write failed, leave sources intact to
        // avoid data loss from a partial copy.
        if (s_workOp == FILEOP_MOVE && s_opCopyOK)
        {
            s_opDelFail = 0;
            for (int i = 0; i < s_clipCount; ++i)
                FileDeleteRecursive(s_clipboard[i].path, s_clipboard[i].isDir);
        }

        s_clipCount = 0;
        s_clipOp = FILEOP_NONE;
        s_opRunning = false;
        // Stay at FOS_IDLE but surface any warnings via the skip/fail counts.
        // The render path checks s_workTruncated, s_opSkipCount, s_opDelFail
        // and draws a warning banner if any are non-zero.
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

void FileExplorer_OnEnter()
{
    s_prevBtns = 0;
    s_skipFirstTick = true;

    // Reset file operation state — stale state from a previous session
    // causes the operations list error when re-entering after other modules.
    s_fosState = FOS_IDLE;
    s_clipOp = FILEOP_NONE;
    s_clipCount = 0;
    s_workCount = 0;
    s_workOp = FILEOP_NONE;
    s_pendingOp = FILEOP_NONE;
    s_cursor = 0;

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

    // Seed MU hotplug snapshot so first tick doesn't false-trigger a reload
    for (int p = 0; p < 4; ++p)
        for (int sl = 0; sl < 2; ++sl)
            s_muSnapshot[p][sl] = IsMUPresent(p, sl);

    s_muFormatPending = false;
    s_muFormatPort = -1;
    s_muFormatSlot = -1;

    // FTP off until [Start]
    ZeroMemory(&g_ftp, sizeof(g_ftp));
    g_ftp.listenSock = INVALID_SOCKET;
    g_ftp.ctrlSock = INVALID_SOCKET;
    g_ftp.dataListen = INVALID_SOCKET;
    g_ftp.dataSock = INVALID_SOCKET;
    g_ftp.xferFile = INVALID_HANDLE_VALUE;
    g_ftp.state = FTP_OFF;
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
    // Check if cursor is on a MU entry at root (sizeLow A-H)
    bool cursorOnMU = s_atRoot && s_entryCount > 0 &&
        s_entries[s_cursor].sizeLow >= (DWORD)'A' &&
        s_entries[s_cursor].sizeLow <= (DWORD)'H';
    if (s_fosState == FOS_CONFIRM_DELETE)
        hints = "[B] Confirm Delete  [Back] Cancel";
    else if (s_fosState == FOS_PICK_DEST)
        hints = "[A] Enter  [Black/White] Confirm  [B] Up / Cancel";
    else if (s_markedCount > 0)
        hints = "[Y] Mark  [Black] Copy  [White] Move  [B] Delete  [Back] Clear";
    else if (s_clipCount > 0)
        hints = "[Black] Paste  [White] Move here  [Back] Clear clipboard";
    else if (cursorOnMU)
        hints = g_ftp.state != FTP_OFF
        ? "[A] Open  [B] Back  [Start] FTP Off  [Back+Black] Format MU"
        : "[A] Open  [B] Back  [Start] FTP On   [Back+Black] Format MU";
    else if (g_ftp.state != FTP_OFF)
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
    if (g_ftp.state != FTP_OFF)
    {
        char ftpBadge[32];
        StrCopy(ftpBadge, sizeof(ftpBadge), "FTP ");
        AppendStr(ftpBadge, sizeof(ftpBadge), s_ipStr);
        DrawTextR(SW - LM, PATH_Y, ftpBadge, 1.1f,
            g_ftp.state == FTP_TRANSFER ? COL_CYAN : COL_GREEN);
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

    // ---- FTP widget (lower right) ----------------------------------------
    FtpServ_DrawWidget();

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

        if (s_fosState == FOS_EXPANDING)
        {
            char scanLine[32];
            char ea[8], eb[8];
            IntToStr(s_expandIdx, ea, sizeof(ea));
            IntToStr(s_clipCount, eb, sizeof(eb));
            StrCopy(scanLine, sizeof(scanLine), "Scanning ");
            StrCat2(scanLine, sizeof(scanLine), scanLine, ea);
            StrCat2(scanLine, sizeof(scanLine), scanLine, " / ");
            StrCat2(scanLine, sizeof(scanLine), scanLine, eb);
            DrawText(OX + OP, ty, scanLine, 1.05f, COL_WHITE);
        }
        else
        {

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
        }  // end else (FOS_RUNNING)
    }

    // ---- Post-operation warning banner ------------------------------------
    // Shown after an op completes if anything was truncated, skipped, or
    // failed to delete.  Stays visible until the user navigates away.
    if (!s_opRunning && s_fosState == FOS_IDLE &&
        (s_workTruncated || s_opSkipCount > 0 || s_opDelFail > 0))
    {
        const float WW = 380.f;
        const float WX = (SW - WW) * 0.5f;
        const float WY = 160.f;
        const float WH = s_workTruncated ? 70.f : 56.f;

        FillRect(WX, WY, WX + WW, WY + WH, D3DCOLOR_ARGB(240, 40, 12, 0));
        HLine(WY, WX, WX + WW, COL_ORANGE);
        HLine(WY + WH, WX, WX + WW, COL_ORANGE);

        float wy = WY + 7.f;
        DrawText(WX + 8.f, wy, "! OPERATION WARNING", 1.15f, COL_ORANGE); wy += 15.f;

        if (s_workTruncated)
        {
            DrawText(WX + 8.f, wy,
                "Work list full — some files were NOT copied.", 1.05f, COL_RED);
            wy += 13.f;
        }
        if (s_opSkipCount > 0)
        {
            char sl[48]; StrCopy(sl, sizeof(sl), "Files skipped (open/write error): ");
            char sc[8];  IntToStr(s_opSkipCount, sc, sizeof(sc));
            StrCat2(sl, sizeof(sl), sl, sc);
            DrawText(WX + 8.f, wy, sl, 1.05f, COL_ORANGE); wy += 13.f;
        }
        if (s_opDelFail > 0)
        {
            char dl[48]; StrCopy(dl, sizeof(dl), "Source delete failures: ");
            char dc[8];  IntToStr(s_opDelFail, dc, sizeof(dc));
            StrCat2(dl, sizeof(dl), dl, dc);
            DrawText(WX + 8.f, wy, dl, 1.05f, COL_ORANGE);
        }
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

    // ---- Destination picker overlay --------------------------------------
    if (s_fosState == FOS_PICK_DEST)
        DrawDestPicker();

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

    // ---- MU format hint — shown when a MU entry is selected at root -------
    if (s_atRoot && s_entryCount > 0 && !s_muFormatPending)
    {
        const FileEntry& ce = s_entries[s_cursor];
        if (ce.isDir && ce.sizeLow >= (DWORD)'A' && ce.sizeLow <= (DWORD)'H')
        {
            DrawText(LM, BOT_BAR_Y - 16.f,
                "[Back+Black] Format Memory Unit",
                1.0f, D3DCOLOR_XRGB(180, 100, 30));
        }
    }

    // ---- MU format confirm overlay ----------------------------------------
    if (s_muFormatPending)
    {
        // Semi-transparent full-screen dim
        FillRect(0.f, 0.f, SW, SH, D3DCOLOR_ARGB(160, 0, 0, 0));

        // Confirm card
        const float CW = 300.f, CH = 110.f;
        const float CX = (SW - CW) * 0.5f, CY = (SH - CH) * 0.5f;
        FillRect(CX, CY, CX + CW, CY + CH, D3DCOLOR_XRGB(14, 17, 38));
        HLine(CY, CX, CX + CW, COL_BORDER);
        HLine(CY + CH, CX, CX + CW, COL_BORDER);
        VLine(CX, CY, CY + CH, COL_BORDER);
        VLine(CX + CW, CY, CY + CH, COL_BORDER);

        DrawText(CX + 10.f, CY + 8.f, "FORMAT MEMORY UNIT", 1.3f, COL_RED);
        DrawText(CX + 10.f, CY + 30.f, s_muFormatLabel, 1.2f, COL_YELLOW);
        DrawText(CX + 10.f, CY + 52.f, "All data will be erased!", 1.1f, COL_GRAY);
        DrawText(CX + 10.f, CY + 74.f, "[A] Format    [B] Cancel", 1.1f, COL_WHITE);
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
    FtpServ_Tick();
    FileOpTick();


    // ---- MU hotplug polling -----------------------------------------------
    // PumpInput is called by the main loop before Tick so IsMUPresent is current.
    // If any slot's presence changed, remount and reload the drive list.
    {
        bool muChanged = false;
        for (int p = 0; p < 4; ++p)
        {
            for (int sl = 0; sl < 2; ++sl)
            {
                bool now = IsMUPresent(p, sl);
                if (now != s_muSnapshot[p][sl])
                {
                    s_muSnapshot[p][sl] = now;
                    muChanged = true;
                    if (now)
                        MountMUs();  // new MU — mount it
                }
            }
        }
        if (muChanged && s_atRoot)
        {
            LoadDriveList();
            s_cursor = 0;
            s_scroll = 0;
        }
    }

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

    // ---- MU format confirm modal — intercepts all input while active -------
    if (s_muFormatPending)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_A))
        {
            s_muFormatPending = false;
            FormatMU(s_muFormatPort, s_muFormatSlot);
            LoadDriveList();
            s_cursor = 0;
            s_scroll = 0;
        }
        else if (EdgeDown(cur, s_prevBtns, BTN_B))
        {
            s_muFormatPending = false;
        }
        s_prevBtns = cur;
        Render(logo);
        return;
    }

    // [Start] toggle FTP
    if (EdgeDown(cur, s_prevBtns, BTN_START))
    {
        if (g_ftp.state == FTP_OFF)
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
                    FtpServ_Start(s_ipStr, s_ipOK);
                else
                {
                    StrCopy(s_ftpMsg, sizeof(s_ftpMsg), "No IP address");
                    s_ftpMsgFrames = 120;
                }
            }
            else
            {
                FtpServ_Start(s_ipStr, s_ipOK);
            }
        }
        else
            FtpServ_Stop();
    }

    // Block navigation input while file op running
    if (s_opRunning) { s_prevBtns = cur; Render(logo); return; }

    // ---- Destination picker input ----------------------------------------
    if (s_fosState == FOS_PICK_DEST)
    {
        // [DPad Down]
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_DOWN))
        {
            if (s_pickCursor < s_pickEntryCount - 1)
            {
                s_pickCursor++;
                if (s_pickCursor >= s_pickScroll + PICK_ROWS_VISIBLE)
                    s_pickScroll++;
            }
        }
        // [DPad Up]
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_UP))
        {
            if (s_pickCursor > 0)
            {
                s_pickCursor--;
                if (s_pickCursor < s_pickScroll)
                    s_pickScroll--;
            }
        }
        // [A] — at root: enter drive
        //       inside dir + has subfolders: navigate into selected subfolder
        //       inside dir + no subfolders: confirm current path as destination
        if (EdgeDown(cur, s_prevBtns, BTN_A))
        {
            if (s_pickAtRoot)
            {
                if (s_pickEntryCount > 0)
                {
                    FileEntry& pe = s_pickEntries[s_pickCursor];
                    char drivePath[8];
                    drivePath[0] = pe.name[0]; drivePath[1] = ':';
                    drivePath[2] = '\\'; drivePath[3] = '\0';
                    PickLoadDirectory(drivePath);
                    s_pickCursor = 0;
                    s_pickScroll = 0;
                }
            }
            else
            {
                if (s_pickEntryCount > 0)
                {
                    // Navigate into selected subfolder
                    char sub[MAX_PATH_LEN];
                    StrCopy(sub, sizeof(sub), s_pickPath);
                    int pl = 0; while (sub[pl]) pl++;
                    if (pl > 0 && sub[pl - 1] != '\\') { sub[pl++] = '\\'; sub[pl] = '\0'; }
                    AppendStr(sub, sizeof(sub), s_pickEntries[s_pickCursor].name);
                    PickLoadDirectory(sub);
                    s_pickCursor = 0;
                    s_pickScroll = 0;
                }
                else
                {
                    // No subfolders — confirm current path
                    SnapMarkedToClipboard(s_pendingOp);
                    FileOpStartTo(s_pickPath);
                    s_fosState = FOS_IDLE;
                    s_pendingOp = FILEOP_NONE;
                }
            }
        }
        // [Black] or [White] — confirm current picker path as destination
        if ((EdgeDown(cur, s_prevBtns, BTN_BLACK) || EdgeDown(cur, s_prevBtns, BTN_WHITE))
            && !s_pickAtRoot)
        {
            SnapMarkedToClipboard(s_pendingOp);
            FileOpStartTo(s_pickPath);
            s_fosState = FOS_IDLE;
            s_pendingOp = FILEOP_NONE;
        }
        // [B] — go up one level, or cancel if at root
        if (EdgeDown(cur, s_prevBtns, BTN_B))
        {
            if (s_pickAtRoot)
            {
                s_fosState = FOS_IDLE;
                s_pendingOp = FILEOP_NONE;
            }
            else
            {
                int pl = 0; while (s_pickPath[pl]) pl++;
                if (pl > 0 && s_pickPath[pl - 1] == '\\') pl--;
                int sep = -1;
                for (int k = pl - 1; k >= 0; --k)
                    if (s_pickPath[k] == '\\') { sep = k; break; }
                if (sep <= 2)
                    PickLoadDriveList();
                else
                {
                    char parent[MAX_PATH_LEN];
                    int n = sep < MAX_PATH_LEN - 1 ? sep : MAX_PATH_LEN - 1;
                    for (int k = 0; k < n; ++k) parent[k] = s_pickPath[k];
                    parent[n] = '\0';
                    PickLoadDirectory(parent);
                }
                s_pickCursor = 0;
                s_pickScroll = 0;
            }
        }

        s_prevBtns = cur;
        Render(logo);
        return;
    }

    // [Back+Black] on a MU entry at root — prompt to format
    if (s_atRoot && (cur & BTN_BACK) && EdgeDown(cur, s_prevBtns, BTN_BLACK))
    {
        if (s_entryCount > 0)
        {
            FileEntry& e = s_entries[s_cursor];
            // MU entries store their drive letter in sizeLow (A-H range)
            if (e.isDir && e.sizeLow >= (DWORD)'A' && e.sizeLow <= (DWORD)'H')
            {
                int mu = (int)(e.sizeLow - 'A');
                s_muFormatPort = mu / 2;
                s_muFormatSlot = mu % 2;
                StrCopy(s_muFormatLabel, sizeof(s_muFormatLabel), e.name);
                s_muFormatPending = true;
            }
        }
    }

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

    // [Black] — copy marked items: open destination picker
    if (EdgeDown(cur, s_prevBtns, BTN_BLACK))
    {
        if (s_fosState == FOS_IDLE && !s_atRoot && s_markedCount > 0)
        {
            s_pendingOp = FILEOP_COPY;
            PickLoadDriveList();
            s_pickCursor = 0;
            s_pickScroll = 0;
            s_fosState = FOS_PICK_DEST;
        }
        else if (s_fosState == FOS_IDLE && !s_atRoot && s_clipCount > 0)
        {
            // No marks but clipboard present — paste here (original behaviour)
            FileOpStart(s_clipOp == FILEOP_NONE ? FILEOP_COPY : s_clipOp);
        }
    }

    // [White] — move marked items: open destination picker
    if (EdgeDown(cur, s_prevBtns, BTN_WHITE))
    {
        if (s_fosState == FOS_IDLE && !s_atRoot && s_markedCount > 0)
        {
            s_pendingOp = FILEOP_MOVE;
            PickLoadDriveList();
            s_pickCursor = 0;
            s_pickScroll = 0;
            s_fosState = FOS_PICK_DEST;
        }
        else if (s_fosState == FOS_IDLE && !s_atRoot && s_clipCount > 0)
        {
            FileOpStart(FILEOP_MOVE);
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

                        FtpServ_Stop();
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