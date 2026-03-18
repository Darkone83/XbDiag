// FileExplorerOps.cpp
// XbDiag - File operations engine (copy/move/delete) and destination picker.
//
// Maintains a flat work-list (s_work[]) built by ExpandToWorkList.
// FileOpTick() processes one chunk per call to keep the render loop responsive.
// The destination picker (FOS_PICK_DEST) uses its own parallel entry list.

#include "FileExplorerOps.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

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
        FE_TruncName(fd.cFileName, e.name, MAX_NAME_LEN - 1, MAX_NAME_LEN);
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
        FileExplorer_LoadDirectory(drivePath);
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
        FileExplorer_LoadDirectory(newPath);
    }

    s_cursor = 0;
    s_scroll = 0;
}

// Navigate up one level
// ============================================================================
// IP resolution (same pattern as SysInfo)
// ============================================================================
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
        FE_AppendStr(src2, sizeof(src2), fd.cFileName);

        StrCopy(dst2, sizeof(dst2), dstPath);
        int dl = 0; while (dst2[dl]) dl++;
        if (dl > 0 && dst2[dl - 1] != '\\') { dst2[dl++] = '\\'; dst2[dl] = '\0'; }
        FE_AppendStr(dst2, sizeof(dst2), fd.cFileName);

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
            FE_AppendStr(child, sizeof(child), fd.cFileName);
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
            FE_AppendStr(dst, sizeof(dst), fname);
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
            FE_TruncName(sep >= 0 ? wi.src + sep + 1 : wi.src,
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

        FileExplorer_LoadDirectory(s_path);
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
        FE_AppendStr(ce.path, sizeof(ce.path), s_entries[i].name);
        ce.isDir = s_entries[i].isDir;
    }
    for (int i = 0; i < MAX_ENTRIES; ++i) s_marked[i] = false;
    s_markedCount = 0;
}

// ============================================================================
// Public API wrappers
// ============================================================================

void FE_Ops_Snap(FileOpType op) { SnapMarkedToClipboard(op); }
void FE_Ops_StartTo(const char* destDir) { FileOpStartTo(destDir); }
void FE_Ops_Start(FileOpType op) { FileOpStart(op); }
void FE_Ops_Tick() { FileOpTick(); }
void FE_Ops_PickLoadDriveList() { PickLoadDriveList(); }
void FE_Ops_PickLoadDir(const char* path) { PickLoadDirectory(path); }
void FE_Ops_DrawPicker() { DrawDestPicker(); }
void FE_Ops_EnterSelected() { EnterSelected(); }
bool FE_Ops_DeleteRecursive(const char* path, bool dir) { return FileDeleteRecursive(path, dir); }

void FE_Ops_MkDir(const char* name)
{
    if (!name || !name[0]) return;

    // Build full path: s_path + '\' + name
    char fullPath[MAX_PATH_LEN];
    StrCopy(fullPath, sizeof(fullPath), s_path);
    int pl = 0; while (fullPath[pl]) pl++;
    if (pl > 0 && fullPath[pl - 1] != '\\') { fullPath[pl++] = '\\'; fullPath[pl] = '\0'; }
    FE_AppendStr(fullPath, sizeof(fullPath), name);

    CreateDirectoryA(fullPath, NULL);

    // Reload directory so the new folder appears
    FileExplorer_LoadDirectory(s_path);

    // Reposition cursor to the newly created folder
    for (int i = 0; i < s_entryCount; ++i)
    {
        const char* a = s_entries[i].name;
        const char* b = name;
        int k = 0;
        while (a[k] && b[k] && a[k] == b[k]) k++;
        if (!a[k] && !b[k])
        {
            s_cursor = i;
            s_scroll = s_cursor - ROWS_VISIBLE / 2;
            if (s_scroll < 0) s_scroll = 0;
            break;
        }
    }
}