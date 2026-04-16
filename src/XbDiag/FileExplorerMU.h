// FileExplorerMU.h
// XbDiag - Memory Unit mounting, formatting, Skeleton Key, and ENDGAME creation.

#pragma once
#include <xtl.h>

// ----------------------------------------------------------------------------
// Render-pump callback type
// FileExplorer sets g_skRenderFn before calling any blocking MU operation so
// progress updates can repaint the screen mid-operation.
// ----------------------------------------------------------------------------

typedef void (*FE_MU_RenderFn)();

// ----------------------------------------------------------------------------
// Progress globals — written by FileExplorerMU, read by FileExplorer render
//
// Download phase  (SK_DOWNLOADING):
//   g_skProgressDone / g_skProgressTotal  — bytes received / content-length
//
// Create phase  (SK_CREATING):
//   g_skFilesDone / g_skFilesTotal        — files completed / total file count
//                                           (covers both extract and copy sub-phases)
//   g_skProgressDone                      — raw bytes (not used by render in this phase)
// ----------------------------------------------------------------------------

extern DWORD           g_skProgressDone;
extern DWORD           g_skProgressTotal;
extern int             g_skFilesDone;
extern int             g_skFilesTotal;
extern FE_MU_RenderFn  g_skRenderFn;

// ----------------------------------------------------------------------------
// MU presence check (kernel wrapper — defined in FileExplorerMU.cpp)
// ----------------------------------------------------------------------------

bool IsMUPresent(int port, int slot);

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

// Mount all HDD partitions (C/E/F/G/X/Y/Z) and any inserted MUs.
// Safe to call repeatedly — IoCreateSymbolicLink is idempotent for HDD links.
bool FE_MU_MountAll();

// Returns the drive letter for a given MU port/slot.
// Uses a safe table that avoids HDD letters (C,E,F,G,X,Y,Z) and DVD (D).
//   port 0-3, slot 0=top slot, slot 1=bottom slot
char FE_MU_Letter(int port, int slot);

// Returns the drive letter assigned to the MU at port/slot.
// Uses a safe table that avoids HDD letters (C,E,F,G,X,Y,Z) and DVD (D).
char FE_MU_Letter(int port, int slot);

// Dismount, format as FATX, and remount the MU at the given port/slot.
// Returns true on success.  Drive letter is rebound before return.
bool FE_MU_Format(int port, int slot);

// ---- Skeleton Key -----------------------------------------------------------

// Returns true if D:\resources\SK.xba exists and is a file.
bool FE_MU_SKXbaPresent();

// Download SK.xba from the server to D:\resources\SK.xba.
// Writes a human-readable error into errMsgOut on failure.
bool FE_MU_DownloadSK(char* errMsgOut, int errMsgLen);

// Format the MU at port/slot, extract SK.xba to a temp dir, copy all files to
// the MU root, then clean up.  Writes a human-readable error on failure.
bool FE_MU_CreateSK(int port, int slot, char* errMsgOut, int errMsgLen);

// ---- ENDGAME ----------------------------------------------------------------

// Returns true if D:\resources\ENDGAME.xba exists and is a file.
bool FE_MU_EGXbaPresent();

// Download ENDGAME.xba from the server to D:\resources\ENDGAME.xba.
// Writes a human-readable error into errMsgOut on failure.
bool FE_MU_DownloadEG(char* errMsgOut, int errMsgLen);

// Format the MU at port/slot, extract ENDGAME.xba to a temp dir, copy all
// files to the MU root, then clean up.  Writes a human-readable error on failure.
bool FE_MU_CreateEG(int port, int slot, char* errMsgOut, int errMsgLen);