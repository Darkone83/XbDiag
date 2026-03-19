#pragma once
// FileEdit.h
// XbDiag - File edit data and operations layer.
//
// Preserves 100% of original file formatting on save.
// Unchanged lines are written from the original raw buffer byte-for-byte,
// including their exact terminator sequence (\r\n or \n or none on last line).
// Only lines that were actually edited get their content replaced; terminators
// are still copied from the original.
//
// New lines inserted via Enter get the same terminator as the line above.
// Deleted lines simply don't appear in the output.

#include "DiagCommon.h"

static const int FE_MAX_LINES = 4096;
static const int FE_MAX_LINE_LEN = 256;

// Per-logical-line descriptor captured from the original raw buffer.
struct FE_LineInfo
{
    int  bufOffset;    // byte offset of line content start in original s_buf
    int  contentLen;   // byte length of line content (excluding terminator)
    int  termLen;      // byte length of terminator (0, 1, or 2)
};

// Load the edit table from the original file buffer.
//   rawBuf      — the malloc'd file buffer (FileViewer's s_buf)
//   rawBufSize  — bytes in rawBuf
//   lineInfo[]  — one entry per logical line, filled by FileViewer pre-pass
//   lineCount   — number of logical lines
void FileEdit_Load(const char* rawBuf, int rawBufSize,
    const FE_LineInfo* lineInfo, int lineCount);

void FileEdit_Unload();

// Set the active editing line (loads content into working buffer, caret at end)
void FileEdit_SetLine(int lineIdx);

// Character operations on the working buffer
void FileEdit_InsertChar(char c);
void FileEdit_Backspace();
void FileEdit_CaretLeft();
void FileEdit_CaretRight();

// Commit working buffer back to the line table for the current edit line.
void FileEdit_CommitLine();

// Insert a new empty line after lineIdx. Returns new line count.
int  FileEdit_InsertLineAfter(int lineIdx);

// Delete line at lineIdx. Returns new line count.
int  FileEdit_DeleteLine(int lineIdx);

// Write file to path, preserving all original bytes for unchanged lines.
// Returns true on success.
bool FileEdit_Save(const char* path);

// ── Query API ─────────────────────────────────────────────────────────────────
const char* FileEdit_GetLine(int lineIdx);
const char* FileEdit_GetWorkBuf();
int         FileEdit_GetCaret();
int         FileEdit_GetEditIdx();
int         FileEdit_GetCount();
bool        FileEdit_IsDirty();
bool        FileEdit_WasSaved();
DWORD       FileEdit_GetLastError();