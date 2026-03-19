#pragma once
// FileViewer.h
// XbDiag - Read-only text/CSV file viewer.
//
// Supports .txt and .csv files only. Caller checks the extension before opening.
// Entire file is loaded into a malloc'd buffer on open (512KB cap).
// Buffer is always freed on exit — both normal (B) and error paths.
//
// Usage:
//   1. Check FileViewer_CanOpen(filename) — returns true for .txt/.csv.
//   2. Call FileViewer_Open(path, filename) to load and enter viewer.
//   3. Each frame, if FileViewer_IsActive(), call FileViewer_Tick().
//      Tick handles input and rendering — no separate call needed.
//   4. B exits the viewer, frees the buffer, and returns control to caller.
//
// Controls:
//   DPad Up/Down  — scroll one line
//   LT / RT       — page up / page down
//   Y             — open in FileEdit editor
//   B             — exit, free buffer, return to explorer
//
// Long lines are wrapped at 80 chars to fit the display width.
// If the file exceeds 512KB a truncation notice is shown (editing disabled).

#include "DiagCommon.h"

// Returns true if the file extension is .txt, .csv, .cfg, or .ini (case-insensitive).
bool FileViewer_CanOpen(const char* filename);

// Load file at path and enter viewer. filename is shown in the title bar.
// Call only after FileViewer_CanOpen returns true.
void FileViewer_Open(const char* path, const char* filename);

// Returns true while the viewer is active.
bool FileViewer_IsActive();

// Call every frame while FileViewer_IsActive(). Handles input + rendering.
void FileViewer_Tick(const DiagLogo& logo);