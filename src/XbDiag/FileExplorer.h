#pragma once
// FileExplorer.h
// XbDiag - File Explorer shared types, state, and helpers.
//
// FileEntry, enums, and all module state are defined in FileExplorer.cpp
// and exposed here as extern so FileExplorerMU and FileExplorerOps can
// access them directly. Inline helpers are shared via this header.

#include "DiagCommon.h"
#include <xtl.h>

// ============================================================================
// Constants
// ============================================================================

#define MAX_ENTRIES     256
#define MAX_PATH_LEN    256
#define MAX_NAME_LEN    64
#define ROWS_VISIBLE    20
#define ROW_H           (LINE_H + 1.f)
#define LIST_Y          74.f
#define PATH_Y          60.f
#define MAX_CLIPBOARD   64
#define COPY_BUF_SIZE   (64*1024)
#define MAX_WORK_ITEMS  2048
#define PICK_ROWS_VISIBLE 14   // rows visible in destination picker

// FTP
#define FTP_CTRL_PORT        21
#define FTP_DATA_PORT_BASE   2024
#define FTP_DATA_PORT_COUNT  32

// ============================================================================
// Shared types
// ============================================================================

struct FileEntry
{
    char  name[MAX_NAME_LEN];
    bool  isDir;
    DWORD sizeLow;
};

enum FileOpType { FILEOP_NONE = 0, FILEOP_COPY, FILEOP_MOVE };
enum FileOpState { FOS_IDLE = 0, FOS_CONFIRM_DELETE, FOS_EXPANDING, FOS_RUNNING, FOS_PICK_DEST, FOS_CONFIRM_OVERWRITE };
enum WorkItemType { WI_MKDIR = 0, WI_FILE };

struct ClipboardEntry
{
    char path[MAX_PATH_LEN];
    bool isDir;
};

struct WorkItem
{
    WorkItemType type;
    char src[MAX_PATH_LEN];
    char dst[MAX_PATH_LEN];
};

// Xbox symbolic link types (used by MU and drive mounting)
typedef struct _XBOX_STRING {
    USHORT Length;
    USHORT MaximumLength;
    char* Buffer;
} XBOX_STRING;

extern "C" LONG WINAPI IoCreateSymbolicLink(XBOX_STRING* symLink, XBOX_STRING* target);
extern "C" LONG WINAPI IoDeleteSymbolicLink(XBOX_STRING* symLink);

// ============================================================================
// Shared state — defined in FileExplorer.cpp
// ============================================================================

// Explorer
extern FileEntry s_entries[MAX_ENTRIES];
extern int       s_entryCount;
extern int       s_cursor;
extern int       s_scroll;
extern bool      s_atRoot;
extern char      s_path[MAX_PATH_LEN];

// Network / FTP
extern char      s_ipStr[20];
extern bool      s_ipOK;
extern char      s_ftpMsg[48];
extern int       s_ftpMsgFrames;
extern int       s_nextDataPort;

// MU
extern bool      s_muSnapshot[4][2];
extern bool      s_muFormatPending;
extern int       s_muFormatPort;
extern int       s_muFormatSlot;
extern char      s_muFormatLabel[16];

// File ops
extern ClipboardEntry s_clipboard[MAX_CLIPBOARD];
extern int            s_clipCount;
extern FileOpType     s_clipOp;
extern bool           s_marked[MAX_ENTRIES];
extern int            s_markedCount;
extern FileOpState    s_fosState;
extern FileEntry      s_pickEntries[MAX_ENTRIES];
extern int            s_pickEntryCount;
extern int            s_pickCursor;
extern int            s_pickScroll;
extern char           s_pickPath[MAX_PATH_LEN];
extern bool           s_pickAtRoot;
extern FileOpType     s_pendingOp;
extern int            s_expandIdx;
extern char           s_expandDstRoot[MAX_PATH_LEN];
extern WorkItem       s_work[MAX_WORK_ITEMS];
extern int            s_workCount;
extern int            s_workIdx;
extern FileOpType     s_workOp;
extern char           s_workDstRoot[MAX_PATH_LEN];
extern bool           s_opRunning;
extern char           s_opSrcName[MAX_NAME_LEN];
extern DWORD          s_opDone;
extern DWORD          s_opTotal;
extern int            s_opItemDone;
extern int            s_opItemTotal;
extern bool           s_workTruncated;
extern int            s_opSkipCount;
extern int            s_opDelFail;
extern bool           s_opCopyOK;

// Per-file overwrite handshake (ops sets, explorer reads/clears)
extern char           s_overwriteFileName[MAX_NAME_LEN]; // filename to show in prompt
extern int            s_overwriteResponse;  // 0=pending, 1=overwrite, 2=skip, 3=cancel all
extern HANDLE         s_opSrcHandle;
extern HANDLE         s_opDstHandle;

// ============================================================================
// Shared inline helpers
// ============================================================================

static inline bool FE_EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

static inline void FE_AppendStr(char* out, int outLen, const char* src)
{
    int i = 0; while (out[i]) i++;
    while (*src && i < outLen - 1) out[i++] = *src++;
    out[i] = '\0';
}

static inline void FE_FormatSize(DWORD bytes, char* buf, int bufLen)
{
    if (bytes < 1024)
    {
        IntToStr((int)bytes, buf, bufLen); FE_AppendStr(buf, bufLen, " B");
    }
    else if (bytes < 1024 * 1024)
    {
        IntToStr((int)(bytes / 1024), buf, bufLen); FE_AppendStr(buf, bufLen, " KB");
    }
    else
    {
        IntToStr((int)(bytes / (1024 * 1024)), buf, bufLen); FE_AppendStr(buf, bufLen, " MB");
    }
}

static inline void FE_TruncName(const char* src, char* dst, int maxChars, int dstLen)
{
    int len = 0; while (src[len]) len++;
    if (len <= maxChars) { StrCopy(dst, dstLen, src); return; }
    int i = 0;
    for (; i < maxChars - 2 && i < dstLen - 3; ++i) dst[i] = src[i];
    dst[i++] = '.'; dst[i++] = '.'; dst[i] = '\0';
}

static inline bool FE_IsXBE(const char* name)
{
    int len = 0; while (name[len]) len++;
    if (len < 4) return false;
    const char* e = name + len - 4;
    return (e[0] == '.' && (e[1] == 'x' || e[1] == 'X') &&
        (e[2] == 'b' || e[2] == 'B') &&
        (e[3] == 'e' || e[3] == 'E'));
}

// ============================================================================
// Public API
// ============================================================================

// Directory loading — also called by FileExplorerOps
void FileExplorer_LoadDirectory(const char* path);
void FileExplorer_LoadDriveList();

void FileExplorer_OnEnter();
void FileExplorer_Tick(const DiagLogo& logo);