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
//  [Black]       With marks: open destination picker (copy). With clipboard: paste here.
//  [White]       With marks: open destination picker (move). With clipboard: move here.
//  [DPad Up/Dn]  Move cursor
//  [LT / RT]     Page up / page down
//  [Start]       Toggle FTP server on / off
//  [Back+Black]  Open MU Utilities card (Format / Skeleton Key / ENDGAME) when cursor is on a MU at root
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
#include "FileExplorerMU.h"
#include "FileExplorerOps.h"
#include "Keyboard.h"
#include "FileViewer.h"
#include "font.h"
#include "input.h"
#include <xtl.h>
#include <winsockx.h>

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

// ============================================================================
// Module state
// ============================================================================

FileEntry s_entries[MAX_ENTRIES];
int       s_entryCount = 0;
int       s_cursor = 0;
int       s_scroll = 0;
bool      s_atRoot = true;
char      s_path[MAX_PATH_LEN] = {};
static bool s_skipFirstTick = true;
static WORD s_prevBtns = 0;
// Set true whenever a MU modal opens so the first input tick is consumed
// before accepting [A]/[B] — prevents analog button re-assertion auto-fire.
static bool s_muModalSkipInput = false;

// ── New Folder via keyboard ────────────────────────────────────────────────
static char s_mkdirBuf[MAX_NAME_LEN];
static void OnMkdirDone(const char* name) { FE_Ops_MkDir(name); }
static void OnMkdirCancel() {}

char      s_ipStr[20] = {};
bool      s_ipOK = false;
char      s_ftpMsg[48] = {};
int       s_ftpMsgFrames = 0;
int       s_nextDataPort = 0;

bool      s_muSnapshot[4][2] = {};
bool      s_muFormatPending = false;
int       s_muFormatPort = -1;
int       s_muFormatSlot = -1;
char      s_muFormatLabel[16] = {};

// MU Utilities card state
enum MUCardItem { MUCARD_FORMAT = 0, MUCARD_SK, MUCARD_EG, MUCARD_COUNT };
static bool s_muCardOpen = false;
static int  s_muCardCursor = 0;

// Skeleton Key / ENDGAME state (shared flow, s_skIsEndgame selects asset)
enum SKState {
    SK_NONE = 0,
    SK_PROMPT_DOWNLOAD,   // .xba not present — ask to download
    SK_DOWNLOADING,       // download in progress
    SK_CONFIRM_CREATE,    // .xba present — confirm create on this MU
    SK_CREATING,          // extract + format + copy in progress
    SK_DONE,              // success
    SK_ERROR              // something went wrong
};
static SKState s_skState = SK_NONE;
static bool    s_skIsEndgame = false;  // false = Skeleton Key, true = ENDGAME
static int     s_skPort = -1;
static int     s_skSlot = -1;
static char    s_skLabel[16] = {};
static char    s_skMsg[128] = {};

// Pointer to current frame's logo — set at start of Tick, used by SK_RenderPump
static const DiagLogo* s_tickLogo = NULL;

ClipboardEntry s_clipboard[MAX_CLIPBOARD] = {};
int            s_clipCount = 0;
FileOpType     s_clipOp = FILEOP_NONE;
bool           s_marked[MAX_ENTRIES] = {};
int            s_markedCount = 0;
FileOpState    s_fosState = FOS_IDLE;
FileEntry      s_pickEntries[MAX_ENTRIES] = {};
int            s_pickEntryCount = 0;
int            s_pickCursor = 0;
int            s_pickScroll = 0;
char           s_pickPath[MAX_PATH_LEN] = {};
bool           s_pickAtRoot = true;
FileOpType     s_pendingOp = FILEOP_NONE;
char           s_overwriteFileName[MAX_NAME_LEN] = {};
int            s_overwriteResponse = 0;
int            s_expandIdx = 0;
char           s_expandDstRoot[MAX_PATH_LEN] = {};
WorkItem       s_work[MAX_WORK_ITEMS] = {};
int            s_workCount = 0;
int            s_workIdx = 0;
FileOpType     s_workOp = FILEOP_NONE;
char           s_workDstRoot[MAX_PATH_LEN] = {};
HANDLE         s_opSrcHandle = INVALID_HANDLE_VALUE;
HANDLE         s_opDstHandle = INVALID_HANDLE_VALUE;
bool           s_opRunning = false;
char           s_opSrcName[MAX_NAME_LEN] = {};
DWORD          s_opDone = 0;
DWORD          s_opTotal = 0;
int            s_opItemDone = 0;
int            s_opItemTotal = 0;
bool           s_workTruncated = false;
int            s_opSkipCount = 0;
int            s_opDelFail = 0;
bool           s_opCopyOK = true;

// ============================================================================
// LoadDriveList / LoadDirectory
// ============================================================================
static void LoadDriveList()
{
    s_entryCount = 0;
    s_atRoot = true;
    s_path[0] = '\0';
    for (int i = 0; i < MAX_ENTRIES; ++i) s_marked[i] = false;
    s_markedCount = 0;

    // Ensure HDD partitions and any inserted MUs are mapped to drive letters
    FE_MU_MountAll();

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
        FE_TruncName(fd.cFileName, e.name, MAX_NAME_LEN - 1, MAX_NAME_LEN);
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


static void GoUp()
{
    if (s_atRoot) { RequestState(MSTATE_MENU); return; }

    int len = 0; while (s_path[len]) len++;
    int end = len;
    if (end > 0 && s_path[end - 1] == '\\') end--;
    int slash = -1;
    for (int i = end - 1; i >= 0; --i)
        if (s_path[i] == '\\') { slash = i; break; }

    if (slash <= 2)
        LoadDriveList();
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
// Snap then start — overwrite prompts handled per-file inside FileOpTick
static void SnapAndStart(FileOpType op, const char* destDir)
{
    FE_Ops_Snap(op);
    FE_Ops_StartTo(destDir);
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
    s_overwriteFileName[0] = '\0';
    s_overwriteResponse = 0;
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

    s_muCardOpen = false;
    s_muCardCursor = 0;
    s_muModalSkipInput = false;
    s_skState = SK_NONE;
    s_skIsEndgame = false;
    s_skPort = -1;
    s_skSlot = -1;
    s_skLabel[0] = '\0';
    s_skMsg[0] = '\0';

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
        FE_IsXBE(s_entries[s_cursor].name) &&
        s_markedCount == 0;
    // Check if cursor is on a MU entry at root (sizeLow A-H)
    bool cursorOnMU = s_atRoot && s_entryCount > 0 &&
        s_entries[s_cursor].sizeLow >= (DWORD)'A' &&
        s_entries[s_cursor].sizeLow <= (DWORD)'H';
    if (s_fosState == FOS_CONFIRM_DELETE)
        hints = "[B] Confirm Delete  [Back] Cancel";
    else if (s_fosState == FOS_CONFIRM_OVERWRITE)
        hints = "[A] Overwrite  [X] Skip  [Back] Cancel all";
    else if (s_fosState == FOS_PICK_DEST)
        hints = "[A] Enter  [Black/White] Confirm  [B] Up / Cancel";
    else if (s_markedCount > 0)
        hints = "[Y] Mark  [Black] Copy  [White] Move  [B] Delete  [Back] Clear";
    else if (s_clipCount > 0)
        hints = "[Black] Paste  [White] Move here  [Back] Clear clipboard";
    else if (cursorOnMU)
        hints = g_ftp.state != FTP_OFF
        ? "[A] Open  [B] Back  [Start] FTP Off  [Back+Black] MU Utilities"
        : "[A] Open  [B] Back  [Start] FTP On   [Back+Black] MU Utilities";
    else if (g_ftp.state != FTP_OFF)
        hints = canLaunch ? "[A] Open  [B] Back  [X] Launch  [Y] Mark  [R3] New Folder  [Start] FTP Off"
        : "[A] Open  [B] Back  [Y] Mark  [R3] New Folder  [L3] View  [Start] FTP Off";
    else
        hints = canLaunch ? "[A] Open  [B] Back  [X] Launch  [Y] Mark  [R3] New Folder  [Start] FTP On"
        : "[A] Open  [B] Back  [Y] Mark  [R3] New Folder  [L3] View  [Start] FTP On";

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
        FE_AppendStr(ftpBadge, sizeof(ftpBadge), s_ipStr);
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
        FE_TruncName(e.name, dispName, NAME_MAX_CHARS, sizeof(dispName));

        DWORD nameCol;
        if (isMarkedRow)      nameCol = D3DCOLOR_XRGB(80, 255, 100);
        else if (selected)    nameCol = COL_WHITE;
        else if (e.isDir)     nameCol = COL_YELLOW;
        else if (FE_IsXBE(e.name)) nameCol = COL_CYAN;
        else                  nameCol = D3DCOLOR_XRGB(180, 180, 180);

        DrawText(NAME_X, ry, dispName, 1.2f, nameCol);

        // Size (files only)
        if (!e.isDir)
        {
            char szBuf[16];
            FE_FormatSize(e.sizeLow, szBuf, sizeof(szBuf));
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
            FE_AppendStr(pctBuf, sizeof(pctBuf), "%");
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
        FE_AppendStr(delLine, sizeof(delLine), cnt);
        FE_AppendStr(delLine, sizeof(delLine), " item(s)?");
        DrawText(DX + 10.f, DY + 12.f, delLine, 1.15f, COL_WHITE);
        DrawText(DX + 10.f, DY + 30.f, "[B] Confirm", 1.05f, D3DCOLOR_XRGB(255, 80, 80));
        DrawText(DX + 10.f, DY + 46.f, "[Back] Cancel", 1.05f, COL_GRAY);
    }

    // ---- Confirm overwrite overlay ----------------------------------------
    if (s_fosState == FOS_CONFIRM_OVERWRITE)
    {
        const float DW = 340.f;
        const float DH = 96.f;
        const float DX = (SW - DW) * 0.5f;
        const float DY = 192.f;

        FillRect(DX, DY, DX + DW, DY + DH, D3DCOLOR_ARGB(240, 20, 20, 8));
        HLine(DY, DX, DX + DW, D3DCOLOR_XRGB(200, 180, 50));
        HLine(DY + DH, DX, DX + DW, D3DCOLOR_XRGB(200, 180, 50));

        DrawText(DX + 10.f, DY + 8.f, "File already exists:", 1.1f, COL_GRAY);
        DrawText(DX + 10.f, DY + 24.f, s_overwriteFileName, 1.15f, COL_WHITE);
        DrawText(DX + 10.f, DY + 46.f, "[A] Overwrite", 1.05f, COL_GREEN);
        DrawText(DX + 10.f, DY + 62.f, "[X] Skip file", 1.05f, COL_YELLOW);
        DrawText(DX + 10.f, DY + 78.f, "[Back] Cancel all", 1.05f, COL_GRAY);
    }

    // ---- Destination picker overlay --------------------------------------
    if (s_fosState == FOS_PICK_DEST)
        FE_Ops_DrawPicker();

    // ---- Clipboard status bar (bottom of list, above bot bar) ------------
    if (s_clipCount > 0 && !s_opRunning && s_fosState == FOS_IDLE)
    {
        char clipLine[64] = "Clipboard: ";
        char cnt[8];
        IntToStr(s_clipCount, cnt, sizeof(cnt));
        FE_AppendStr(clipLine, sizeof(clipLine), cnt);
        FE_AppendStr(clipLine, sizeof(clipLine), " item(s)  ");
        FE_AppendStr(clipLine, sizeof(clipLine), s_clipOp == FILEOP_MOVE ? "[MOVE]" : "[COPY]");
        DrawText(LM, BOT_BAR_Y - 16.f, clipLine, 1.05f, COL_CYAN);
    }

    // ---- MU Utilities hint — shown when a MU entry is selected at root ------
    if (s_atRoot && s_entryCount > 0 && !s_muFormatPending && !s_muCardOpen)
    {
        const FileEntry& ce = s_entries[s_cursor];
        if (ce.isDir && ce.sizeLow >= (DWORD)'A' && ce.sizeLow <= (DWORD)'H')
        {
            DrawText(LM, BOT_BAR_Y - 16.f,
                "[Back+Black] MU Utilities",
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

    // ---- MU Utilities card -----------------------------------------------
    if (s_muCardOpen && !s_muFormatPending)
    {
        FillRect(0.f, 0.f, SW, SH, D3DCOLOR_ARGB(160, 0, 0, 0));

        const float CW = 300.f, CH = 136.f;
        const float CX = (SW - CW) * 0.5f, CY = (SH - CH) * 0.5f;
        const float ROW = 28.f;
        const float PADY = 36.f;

        FillRect(CX, CY, CX + CW, CY + CH, D3DCOLOR_XRGB(14, 17, 38));
        HLine(CY, CX, CX + CW, COL_CYAN);
        HLine(CY + CH, CX, CX + CW, COL_BORDER);
        VLine(CX, CY, CY + CH, COL_BORDER);
        VLine(CX + CW, CY, CY + CH, COL_BORDER);

        DrawText(CX + 10.f, CY + 8.f, "MU UTILITIES", 1.3f, COL_CYAN);
        DrawText(CX + 10.f, CY + 22.f, s_muFormatLabel, 1.05f, COL_YELLOW);

        static const char* k_cardLabels[MUCARD_COUNT] = {
            "Format",
            "Create Skeleton Key",
            "Create ENDGAME"
        };

        for (int i = 0; i < MUCARD_COUNT; ++i)
        {
            float iy = CY + PADY + (float)i * ROW;
            if (i == s_muCardCursor)
                FillRect(CX + 4.f, iy - 1.f, CX + CW - 4.f, iy + ROW - 4.f,
                    D3DCOLOR_XRGB(30, 50, 120));
            DrawText(CX + 14.f, iy + 4.f, k_cardLabels[i], 1.1f,
                i == s_muCardCursor ? COL_WHITE : COL_GRAY);
        }

        DrawText(CX + 10.f, CY + CH - 16.f,
            "[A] Select    [B] Cancel", 1.0f, COL_DIM);
    }

    // ---- Skeleton Key overlay --------------------------------------------
    if (s_skState != SK_NONE)
    {
        FillRect(0.f, 0.f, SW, SH, D3DCOLOR_ARGB(160, 0, 0, 0));

        const float CW = 400.f, CH = 180.f;
        const float CX = (SW - CW) * 0.5f, CY = (SH - CH) * 0.5f;
        // Bottom-anchored progress bar region: 10px bar + 14px label, 12px bottom padding
        const float BAR_H = 10.f;
        const float BAR_BOT = CY + CH - 12.f;          // bottom border of bar
        const float BAR_TOP = BAR_BOT - BAR_H;          // top border of bar
        const float LBL_Y = BAR_TOP - 14.f;           // "Files: N" / "X / Y KB" label
        const float BX = CX + 10.f;
        const float BW = CW - 20.f;
        FillRect(CX, CY, CX + CW, CY + CH, D3DCOLOR_XRGB(14, 17, 38));
        HLine(CY, CX, CX + CW, COL_CYAN);
        HLine(CY + CH, CX, CX + CW, COL_BORDER);
        VLine(CX, CY, CY + CH, COL_BORDER);
        VLine(CX + CW, CY, CY + CH, COL_BORDER);

        DrawText(CX + 10.f, CY + 8.f,
            s_skIsEndgame ? "ENDGAME" : "SKELETON KEY", 1.3f, COL_CYAN);
        DrawText(CX + 10.f, CY + 28.f, s_skLabel, 1.1f, COL_YELLOW);

        if (s_skState == SK_PROMPT_DOWNLOAD)
        {
            DrawText(CX + 10.f, CY + 52.f,
                s_skIsEndgame
                ? "ENDGAME.xba not found. Download from server?"
                : "SK.xba not found. Download from server?",
                1.05f, COL_WHITE);
            DrawText(CX + 10.f, CY + 72.f, "[A] Download    [B] Cancel", 1.1f, COL_WHITE);
        }
        else if (s_skState == SK_DOWNLOADING)
        {
            DrawText(CX + 10.f, CY + 52.f,
                s_skIsEndgame ? "Downloading ENDGAME.xba..." : "Downloading SK.xba...",
                1.1f, COL_GRAY);
            // Byte count label above the bar
            {
                char szBuf[32];
                char done[12], total[12];
                IntToStr((int)(g_skProgressDone / 1024), done, sizeof(done));
                IntToStr((int)(g_skProgressTotal / 1024), total, sizeof(total));
                StrCopy(szBuf, sizeof(szBuf), done);
                StrCat2(szBuf, sizeof(szBuf), szBuf, " / ");
                StrCat2(szBuf, sizeof(szBuf), szBuf, total);
                StrCat2(szBuf, sizeof(szBuf), szBuf, " KB");
                DrawText(BX, LBL_Y, szBuf, 1.0f, COL_DIM);
            }
            // Progress bar — bottom-anchored
            {
                float frac = (g_skProgressTotal > 0)
                    ? (float)g_skProgressDone / (float)g_skProgressTotal : 0.f;
                if (frac > 1.f) frac = 1.f;
                FillRect(BX, BAR_TOP, BX + BW, BAR_BOT, D3DCOLOR_XRGB(14, 17, 38));
                if (frac > 0.f)
                    FillRectGrad(BX, BAR_TOP, BX + BW * frac, BAR_BOT,
                        D3DCOLOR_XRGB(40, 160, 255), D3DCOLOR_XRGB(20, 100, 200));
                HLine(BAR_TOP, BX, BX + BW, COL_BORDER);
                HLine(BAR_BOT, BX, BX + BW, COL_BORDER);
            }
        }
        else if (s_skState == SK_CONFIRM_CREATE)
        {
            DrawText(CX + 10.f, CY + 52.f,
                s_skIsEndgame
                ? "Format MU and write ENDGAME?"
                : "Format MU and write Skeleton Key?",
                1.05f, COL_WHITE);
            DrawText(CX + 10.f, CY + 70.f, "All MU data will be erased!", 1.05f, COL_GRAY);
            DrawText(CX + 10.f, CY + 96.f, "[A] Create    [B] Cancel", 1.1f, COL_WHITE);
        }
        else if (s_skState == SK_CREATING)
        {
            DrawText(CX + 10.f, CY + 52.f,
                s_skIsEndgame ? "Creating ENDGAME MU..." : "Creating Skeleton Key...",
                1.1f, COL_GRAY);
            // "N / M files" label above the bar
            {
                char fBuf[32]; char fa[8], fb[8];
                IntToStr(g_skFilesDone, fa, sizeof(fa));
                IntToStr(g_skFilesTotal, fb, sizeof(fb));
                StrCopy(fBuf, sizeof(fBuf), fa);
                StrCat2(fBuf, sizeof(fBuf), fBuf, " / ");
                StrCat2(fBuf, sizeof(fBuf), fBuf, fb);
                StrCat2(fBuf, sizeof(fBuf), fBuf, " files");
                DrawText(BX, LBL_Y, fBuf, 1.0f, COL_DIM);
            }
            // File count progress bar — bottom-anchored
            {
                float frac = (g_skFilesTotal > 0)
                    ? (float)g_skFilesDone / (float)g_skFilesTotal : 0.f;
                if (frac > 1.f) frac = 1.f;
                FillRect(BX, BAR_TOP, BX + BW, BAR_BOT, D3DCOLOR_XRGB(14, 17, 38));
                if (frac > 0.f)
                    FillRectGrad(BX, BAR_TOP, BX + BW * frac, BAR_BOT,
                        D3DCOLOR_XRGB(40, 200, 80), D3DCOLOR_XRGB(20, 120, 40));
                HLine(BAR_TOP, BX, BX + BW, COL_BORDER);
                HLine(BAR_BOT, BX, BX + BW, COL_BORDER);
            }
        }
        else if (s_skState == SK_DONE)
        {
            DrawText(CX + 10.f, CY + 52.f,
                s_skIsEndgame ? "ENDGAME MU created!" : "Skeleton Key created!",
                1.15f, COL_GREEN);
            DrawText(CX + 10.f, CY + 76.f, "[B] Close", 1.1f, COL_WHITE);
        }
        else if (s_skState == SK_ERROR)
        {
            // Split at ': ' if present to show type on line 1, detail on line 2
            const char* detail = NULL;
            char line1[64]; StrCopy(line1, sizeof(line1), s_skMsg);
            int mi = 0;
            while (s_skMsg[mi] && !(s_skMsg[mi] == ':' && s_skMsg[mi + 1] == ' ')) ++mi;
            if (s_skMsg[mi] == ':')
            {
                line1[mi] = '\0';
                detail = s_skMsg + mi + 2;
            }
            DrawText(CX + 10.f, CY + 52.f, line1, 1.05f, COL_RED);
            if (detail && detail[0])
                DrawText(CX + 10.f, CY + 70.f, detail, 1.0f, COL_ORANGE);
            DrawText(CX + 10.f, CY + 100.f, "[B] Close", 1.1f, COL_WHITE);
        }
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// SK_RenderPump — called from FileExplorerMU during blocking SK operations
// ============================================================================

static void SK_RenderPump()
{
    // Mirror Update.cpp: call Render() directly — it owns BeginScene/EndScene/Present.
    // Called from inside blocking SK operations (download, extract, copy).
    // The natural pace of recv()/WriteFile()/ReadFile() throttles frame rate
    // without any explicit timer needed.
    if (!s_tickLogo) return;
    Render(*s_tickLogo);
}

// ============================================================================
// FileExplorer_Tick
// ============================================================================

void FileExplorer_Tick(const DiagLogo& logo)
{
    s_tickLogo = &logo;
    // Service file ops every tick regardless of input skip.
    // FtpServ_Tick is now called from the main loop so it runs across all screens.
    FE_Ops_Tick();

    // Keyboard and FileViewer intercept all input while open
    if (Keyboard_IsActive()) { Keyboard_Tick(logo);  s_prevBtns = GetButtons(); return; }
    if (FileViewer_IsActive()) { FileViewer_Tick(logo); s_prevBtns = GetButtons(); return; }


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
                        FE_MU_MountAll();  // new MU — mount it
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
        if (s_muModalSkipInput)
        {
            s_muModalSkipInput = false;
            s_prevBtns = cur;
            Render(logo);
            return;
        }
        if (FE_EdgeDown(cur, s_prevBtns, BTN_A))
        {
            s_muFormatPending = false;
            FE_MU_Format(s_muFormatPort, s_muFormatSlot);
            LoadDriveList();
            s_cursor = 0;
            s_scroll = 0;
            // Mask [A] so the next card open doesn't inherit this press.
            s_prevBtns = cur & ~BTN_A;
            Render(logo);
            return;
        }
        else if (FE_EdgeDown(cur, s_prevBtns, BTN_B))
        {
            s_muFormatPending = false;
            // Mask [B] so it doesn't also trigger go-up on the same edge.
            s_prevBtns = cur & ~BTN_B;
            Render(logo);
            return;
        }
        s_prevBtns = cur;
        Render(logo);
        return;
    }

    // ---- MU Utilities card modal — intercepts all input while open ----------
    if (s_muCardOpen)
    {
        if (s_muModalSkipInput)
        {
            s_muModalSkipInput = false;
            s_prevBtns = cur;
            Render(logo);
            return;
        }
        if (FE_EdgeDown(cur, s_prevBtns, BTN_DPAD_UP))
        {
            if (s_muCardCursor > 0) s_muCardCursor--;
        }
        else if (FE_EdgeDown(cur, s_prevBtns, BTN_DPAD_DOWN))
        {
            if (s_muCardCursor < MUCARD_COUNT - 1) s_muCardCursor++;
        }
        else if (FE_EdgeDown(cur, s_prevBtns, BTN_A))
        {
            s_muCardOpen = false;
            if (s_muCardCursor == MUCARD_FORMAT)
            {
                s_muFormatPending = true;
            }
            else
            {
                // MUCARD_SK or MUCARD_EG — enter the shared SK/EG flow
                s_skIsEndgame = (s_muCardCursor == MUCARD_EG);
                s_skMsg[0] = '\0';
                if (s_skIsEndgame)
                    s_skState = FE_MU_EGXbaPresent() ? SK_CONFIRM_CREATE : SK_PROMPT_DOWNLOAD;
                else
                    s_skState = FE_MU_SKXbaPresent() ? SK_CONFIRM_CREATE : SK_PROMPT_DOWNLOAD;
            }
            // Mask [A] from s_prevBtns so whichever modal opens next doesn't
            // inherit this press and fire immediately on the same edge.
            // Also set skip so the new modal burns one tick before accepting input
            // — analog [A] re-asserts while still physically held.
            s_muModalSkipInput = true;
            s_prevBtns = cur & ~BTN_A;
            Render(logo);
            return;
        }
        else if (FE_EdgeDown(cur, s_prevBtns, BTN_B))
        {
            s_muCardOpen = false;
        }
        s_prevBtns = cur;
        Render(logo);
        return;
    }

    if (s_skState != SK_NONE)
    {
        if (s_muModalSkipInput)
        {
            s_muModalSkipInput = false;
            s_prevBtns = cur;
            Render(logo);
            return;
        }
        if (s_skState == SK_PROMPT_DOWNLOAD)
        {
            if (FE_EdgeDown(cur, s_prevBtns, BTN_A))
            {
                s_skState = SK_DOWNLOADING;
                s_prevBtns = cur;
                // Show downloading frame then pump during blocking call
                Render(logo);

                g_skRenderFn = SK_RenderPump;
                bool ok = s_skIsEndgame
                    ? FE_MU_DownloadEG(s_skMsg, sizeof(s_skMsg))
                    : FE_MU_DownloadSK(s_skMsg, sizeof(s_skMsg));
                g_skRenderFn = NULL;
                if (ok)
                    s_skState = SK_CONFIRM_CREATE;
                else
                    s_skState = SK_ERROR;
            }
            else if (FE_EdgeDown(cur, s_prevBtns, BTN_B))
            {
                s_skState = SK_NONE;
                s_prevBtns = cur & ~BTN_B;
                Render(logo);
                return;
            }
        }
        else if (s_skState == SK_CONFIRM_CREATE)
        {
            if (FE_EdgeDown(cur, s_prevBtns, BTN_A))
            {
                s_skState = SK_CREATING;
                s_prevBtns = cur;
                // Show creating frame then pump during blocking call
                Render(logo);

                g_skRenderFn = SK_RenderPump;
                bool ok = s_skIsEndgame
                    ? FE_MU_CreateEG(s_skPort, s_skSlot, s_skMsg, sizeof(s_skMsg))
                    : FE_MU_CreateSK(s_skPort, s_skSlot, s_skMsg, sizeof(s_skMsg));
                g_skRenderFn = NULL;
                s_skState = ok ? SK_DONE : SK_ERROR;
                if (ok)
                {
                    LoadDriveList();
                    s_cursor = 0; s_scroll = 0;
                }
            }
            else if (FE_EdgeDown(cur, s_prevBtns, BTN_B))
            {
                s_skState = SK_NONE;
                s_prevBtns = cur & ~BTN_B;
                Render(logo);
                return;
            }
        }
        else if (s_skState == SK_DONE || s_skState == SK_ERROR)
        {
            if (FE_EdgeDown(cur, s_prevBtns, BTN_B))
            {
                s_skState = SK_NONE;
                // Mask [B] so it doesn't also fire go-up in the main flow.
                s_prevBtns = cur & ~BTN_B;
                Render(logo);
                return;
            }
        }
        // SK_DOWNLOADING and SK_CREATING are blocking — no input processed.
        s_prevBtns = cur;
        Render(logo);
        return;
    }

    // [Start] toggle FTP
    if (FE_EdgeDown(cur, s_prevBtns, BTN_START))
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
    if (s_opRunning && s_fosState != FOS_CONFIRM_OVERWRITE) { s_prevBtns = cur; Render(logo); return; }

    // ---- Destination picker input ----------------------------------------
    if (s_fosState == FOS_PICK_DEST)
    {
        // [DPad Down]
        if (FE_EdgeDown(cur, s_prevBtns, BTN_DPAD_DOWN))
        {
            if (s_pickCursor < s_pickEntryCount - 1)
            {
                s_pickCursor++;
                if (s_pickCursor >= s_pickScroll + PICK_ROWS_VISIBLE)
                    s_pickScroll++;
            }
        }
        // [DPad Up]
        if (FE_EdgeDown(cur, s_prevBtns, BTN_DPAD_UP))
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
        if (FE_EdgeDown(cur, s_prevBtns, BTN_A))
        {
            if (s_pickAtRoot)
            {
                if (s_pickEntryCount > 0)
                {
                    FileEntry& pe = s_pickEntries[s_pickCursor];
                    // MU entries store the drive letter in sizeLow (A-H);
                    // HDD entries have sizeLow==0 so fall back to name[0].
                    char driveLetter = (pe.sizeLow >= (DWORD)'A' && pe.sizeLow <= (DWORD)'H')
                        ? (char)pe.sizeLow : pe.name[0];
                    char drivePath[8];
                    drivePath[0] = driveLetter; drivePath[1] = ':';
                    drivePath[2] = '\\'; drivePath[3] = '\0';
                    FE_Ops_PickLoadDir(drivePath);
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
                    FE_AppendStr(sub, sizeof(sub), s_pickEntries[s_pickCursor].name);
                    FE_Ops_PickLoadDir(sub);
                    s_pickCursor = 0;
                    s_pickScroll = 0;
                }
                else
                {
                    // No subfolders — confirm current path as destination
                    FileOpType opToRun = s_pendingOp;
                    s_fosState = FOS_IDLE;
                    s_pendingOp = FILEOP_NONE;
                    SnapAndStart(opToRun, s_pickPath);
                }
            }
        }
        // [Black] or [White] — confirm current picker path as destination
        if (FE_EdgeDown(cur, s_prevBtns, BTN_BLACK) || FE_EdgeDown(cur, s_prevBtns, BTN_WHITE))
        {
            const char* destPath = s_pickPath;
            char driveFallback[8];
            if (s_pickAtRoot && s_pickEntryCount > 0)
            {
                // At root — use the selected drive as destination.
                // MU entries store the drive letter in sizeLow (A-H);
                // HDD entries have sizeLow==0 so fall back to name[0].
                FileEntry& pe = s_pickEntries[s_pickCursor];
                char driveLetter = (pe.sizeLow >= (DWORD)'A' && pe.sizeLow <= (DWORD)'H')
                    ? (char)pe.sizeLow : pe.name[0];
                driveFallback[0] = driveLetter;
                driveFallback[1] = ':'; driveFallback[2] = '\\'; driveFallback[3] = '\0';
                destPath = driveFallback;
            }
            // Close picker first so FOS_EXPANDING render works correctly
            FileOpType opToRun = s_pendingOp;
            s_fosState = FOS_IDLE;
            s_pendingOp = FILEOP_NONE;
            if (destPath[0] != '\0')
                SnapAndStart(opToRun, destPath);
        }
        // [B] — go up one level, or cancel if at root
        if (FE_EdgeDown(cur, s_prevBtns, BTN_B))
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
                    FE_Ops_PickLoadDriveList();
                else
                {
                    char parent[MAX_PATH_LEN];
                    int n = sep < MAX_PATH_LEN - 1 ? sep : MAX_PATH_LEN - 1;
                    for (int k = 0; k < n; ++k) parent[k] = s_pickPath[k];
                    parent[n] = '\0';
                    FE_Ops_PickLoadDir(parent);
                }
                s_pickCursor = 0;
                s_pickScroll = 0;
            }
        }

        s_prevBtns = cur;
        Render(logo);
        return;
    }

    // [Back+Black] on a MU entry at root — open MU Utilities card
    if (s_atRoot && (cur & BTN_BACK) && FE_EdgeDown(cur, s_prevBtns, BTN_BLACK))
    {
        if (s_entryCount > 0)
        {
            FileEntry& e = s_entries[s_cursor];
            if (e.isDir && e.sizeLow >= (DWORD)'A' && e.sizeLow <= (DWORD)'H')
            {
                int mu = (int)(e.sizeLow - 'A');
                s_muFormatPort = mu / 2;
                s_muFormatSlot = mu % 2;
                s_skPort = mu / 2;
                s_skSlot = mu % 2;
                StrCopy(s_muFormatLabel, sizeof(s_muFormatLabel), e.name);
                StrCopy(s_skLabel, sizeof(s_skLabel), e.name);
                s_muCardCursor = 0;
                s_muCardOpen = true;
                s_muModalSkipInput = true;
                // Mask Back+Black out of s_prevBtns so neither button is seen
                // as held when the card modal processes input on the next tick.
                s_prevBtns = cur & ~(BTN_BACK | BTN_BLACK);
                Render(logo);
                return;
            }
        }
    }

    // [DPad Down]
    if (FE_EdgeDown(cur, s_prevBtns, BTN_DPAD_DOWN))
    {
        if (s_cursor < s_entryCount - 1)
        {
            s_cursor++;
            if (s_cursor >= s_scroll + ROWS_VISIBLE)
                s_scroll = s_cursor - ROWS_VISIBLE + 1;
        }
    }

    // [DPad Up]
    if (FE_EdgeDown(cur, s_prevBtns, BTN_DPAD_UP))
    {
        if (s_cursor > 0)
        {
            s_cursor--;
            if (s_cursor < s_scroll)
                s_scroll = s_cursor;
        }
    }

    // [LT] page up
    if (FE_EdgeDown(cur, s_prevBtns, BTN_LTRIG))
    {
        s_cursor -= ROWS_VISIBLE;
        if (s_cursor < 0) s_cursor = 0;
        s_scroll = s_cursor;
        if (s_scroll > s_cursor - ROWS_VISIBLE + 1)
            s_scroll = s_cursor - ROWS_VISIBLE + 1;
        if (s_scroll < 0) s_scroll = 0;
    }

    // [RT] page down
    if (FE_EdgeDown(cur, s_prevBtns, BTN_RTRIG))
    {
        s_cursor += ROWS_VISIBLE;
        if (s_cursor >= s_entryCount) s_cursor = s_entryCount - 1;
        s_scroll = s_cursor - ROWS_VISIBLE + 1;
        if (s_scroll < 0) s_scroll = 0;
    }

    // [A] enter / open
    if (FE_EdgeDown(cur, s_prevBtns, BTN_A))
    {
        if (s_entryCount > 0)
        {
            FileEntry& e = s_entries[s_cursor];
            if (e.isDir || s_atRoot)
                FE_Ops_EnterSelected();
            // [A] on a file does nothing — use [X] to launch XBEs
        }
    }

    // [Y] mark / unmark current item
    if (FE_EdgeDown(cur, s_prevBtns, BTN_Y))
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

    // [Black] — with marks: open destination picker for copy
    //           with clipboard: paste into current directory
    if (FE_EdgeDown(cur, s_prevBtns, BTN_BLACK))
    {
        if (s_fosState == FOS_IDLE && !s_atRoot)
        {
            if (s_markedCount > 0)
            {
                s_pendingOp = FILEOP_COPY;
                FE_Ops_PickLoadDriveList();
                s_pickCursor = 0;
                s_pickScroll = 0;
                s_fosState = FOS_PICK_DEST;
            }
            else if (s_clipCount > 0)
            {
                FE_Ops_Start(s_clipOp == FILEOP_NONE ? FILEOP_COPY : s_clipOp);
            }
        }
    }

    // [White] — with marks: open destination picker for move
    //           with clipboard: move into current directory
    if (FE_EdgeDown(cur, s_prevBtns, BTN_WHITE))
    {
        if (s_fosState == FOS_IDLE && !s_atRoot)
        {
            if (s_markedCount > 0)
            {
                s_pendingOp = FILEOP_MOVE;
                FE_Ops_PickLoadDriveList();
                s_pickCursor = 0;
                s_pickScroll = 0;
                s_fosState = FOS_PICK_DEST;
            }
            else if (s_clipCount > 0)
            {
                FE_Ops_Start(FILEOP_MOVE);
            }
        }
    }
    // ---- Overwrite confirm input -----------------------------------------
    if (s_fosState == FOS_CONFIRM_OVERWRITE)
    {
        if (FE_EdgeDown(cur, s_prevBtns, BTN_A))
            s_overwriteResponse = 1;  // overwrite this file
        else if (FE_EdgeDown(cur, s_prevBtns, BTN_X))
            s_overwriteResponse = 2;  // skip this file
        else if (FE_EdgeDown(cur, s_prevBtns, BTN_BACK))
            s_overwriteResponse = 3;  // cancel entire op

        s_prevBtns = cur;
        Render(logo);
        return;
    }

    // [B] — confirm delete if pending, else go up / back to menu
    if (FE_EdgeDown(cur, s_prevBtns, BTN_B))
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
                FE_AppendStr(fullPath, sizeof(fullPath), s_entries[i].name);
                FE_Ops_DeleteRecursive(fullPath, s_entries[i].isDir);
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
    if (FE_EdgeDown(cur, s_prevBtns, BTN_BACK))
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
    if (FE_EdgeDown(cur, s_prevBtns, BTN_X))
    {
        if (!s_atRoot && s_entryCount > 0 && s_markedCount == 0)
        {
            FileEntry& e = s_entries[s_cursor];
            if (e.isDir)
            {
                // directories silently ignore — hint bar already hides [X] for dirs
            }
            else if (!FE_IsXBE(e.name))
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
                        FE_AppendStr(launchPath, sizeof(launchPath), e.name);

                        FtpServ_Stop();
                        XLaunchNewImage(launchPath, NULL);
                        // Never returns
                    }
                }
            }
        }
    }

    // [R3] — new folder (only inside a directory, not during any op)
    if (FE_EdgeDown(cur, s_prevBtns, BTN_RTHUMB))
    {
        if (!s_atRoot && s_fosState == FOS_IDLE && !s_opRunning)
        {
            s_mkdirBuf[0] = '\0';
            Keyboard_Open("NEW FOLDER", s_mkdirBuf, MAX_NAME_LEN - 1,
                OnMkdirDone, OnMkdirCancel);
        }
    }

    // [L3] — open file viewer for .txt / .csv files
    if (FE_EdgeDown(cur, s_prevBtns, BTN_LTHUMB))
    {
        if (!s_atRoot && s_entryCount > 0 && s_fosState == FOS_IDLE && !s_opRunning)
        {
            FileEntry& e = s_entries[s_cursor];
            if (!e.isDir && FileViewer_CanOpen(e.name))
            {
                char fullPath[MAX_PATH_LEN];
                StrCopy(fullPath, sizeof(fullPath), s_path);
                int pl = 0; while (fullPath[pl]) pl++;
                if (pl > 0 && fullPath[pl - 1] != '\\')
                {
                    fullPath[pl++] = '\\'; fullPath[pl] = '\0';
                }
                FE_AppendStr(fullPath, sizeof(fullPath), e.name);
                FileViewer_Open(fullPath, e.name);
            }
        }
    }

    s_prevBtns = cur;
    Render(logo);
}

void FileExplorer_LoadDirectory(const char* path) { LoadDirectory(path); }
void FileExplorer_LoadDriveList() { LoadDriveList(); }