// FileEdit.cpp
// XbDiag - File edit data and operations layer.
// See FileEdit.h for full API and usage notes.
//
// Save strategy — byte-perfect output:
//   Each logical line has a FE_LineInfo describing its location in the
//   original raw buffer.  On save we walk the line table in order:
//     - If line is UNCHANGED: copy rawBuf[offset .. offset+contentLen]
//       then copy rawBuf[offset+contentLen .. offset+contentLen+termLen]
//       (the exact original terminator bytes)
//     - If line is CHANGED (s_edited[i] == true): write s_lines[i] content,
//       then copy the original terminator bytes for that line
//     - If line was INSERTED (bufOffset == -1): write content then write
//       the terminator of the line above it (or \r\n if first line)
//   Lines marked deleted are simply skipped.
//   This guarantees every byte outside the edited content is identical to
//   the original file.

#include "FileEdit.h"
#include <xtl.h>
#include <stdlib.h>

// ============================================================================
// State
// ============================================================================

// Original raw buffer — pointer only, NOT owned by FileEdit
static const char* s_rawBuf = NULL;
static int         s_rawBufSize = 0;

// Per-line info from original buffer
static FE_LineInfo s_info[FE_MAX_LINES];

// Editable line content
static char s_lines[FE_MAX_LINES][FE_MAX_LINE_LEN];
static bool s_edited[FE_MAX_LINES];   // true = content differs from original
static bool s_deleted[FE_MAX_LINES];  // true = line removed

static int  s_lineCount = 0;          // total lines including deleted ones

static bool s_dirty = false;
static bool s_wasSaved = false;
static DWORD s_lastError = 0;

// Working buffer for current line being edited
static char s_workBuf[FE_MAX_LINE_LEN];
static int  s_caret = 0;
static int  s_editIdx = -1;

// ============================================================================
// Helpers
// ============================================================================

static int FE_Len(const char* s)
{
    int n = 0; while (s[n]) n++; return n;
}

static void FE_Copy(char* dst, const char* src, int maxLen)
{
    int i = 0;
    while (i < maxLen - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// Default terminator: scan back from lineIdx to find most recent non-inserted line
static void WriteDefaultTerm(HANDLE hFile, int lineIdx)
{
    for (int i = lineIdx; i >= 0; --i)
    {
        if (s_info[i].bufOffset >= 0 && s_info[i].termLen > 0)
        {
            DWORD w = 0;
            WriteFile(hFile,
                s_rawBuf + s_info[i].bufOffset + s_info[i].contentLen,
                (DWORD)s_info[i].termLen, &w, NULL);
            return;
        }
    }
    // Fallback: write \r\n
    DWORD w = 0;
    WriteFile(hFile, "\r\n", 2, &w, NULL);
}

// ============================================================================
// FileEdit_Load
// ============================================================================

void FileEdit_Load(const char* rawBuf, int rawBufSize,
    const FE_LineInfo* lineInfo, int lineCount)
{
    s_rawBuf = rawBuf;
    s_rawBufSize = rawBufSize;
    s_lineCount = lineCount < FE_MAX_LINES ? lineCount : FE_MAX_LINES;
    s_dirty = false;
    s_wasSaved = false;
    s_lastError = 0;
    s_editIdx = -1;
    s_caret = 0;
    s_workBuf[0] = '\0';

    for (int i = 0; i < s_lineCount; ++i)
    {
        s_info[i] = lineInfo[i];
        s_edited[i] = false;
        s_deleted[i] = false;

        // Copy content from raw buffer as initial line text
        int off = lineInfo[i].bufOffset;
        int len = lineInfo[i].contentLen;
        if (off >= 0 && len > 0 && rawBuf)
        {
            int copy = len < FE_MAX_LINE_LEN - 1 ? len : FE_MAX_LINE_LEN - 1;
            for (int j = 0; j < copy; ++j)
                s_lines[i][j] = rawBuf[off + j];
            s_lines[i][copy] = '\0';
        }
        else
        {
            s_lines[i][0] = '\0';
        }
    }
}

// ============================================================================
// FileEdit_Unload
// ============================================================================

void FileEdit_Unload()
{
    s_rawBuf = NULL;
    s_rawBufSize = 0;
    s_lineCount = 0;
    s_dirty = false;
    s_editIdx = -1;
    s_caret = 0;
    s_workBuf[0] = '\0';
    // s_wasSaved intentionally not cleared
}

// ============================================================================
// FileEdit_SetLine
// ============================================================================

void FileEdit_SetLine(int lineIdx)
{
    // Skip deleted lines
    while (lineIdx < s_lineCount && s_deleted[lineIdx]) lineIdx++;
    if (lineIdx >= s_lineCount) return;

    if (s_editIdx >= 0 && s_editIdx < s_lineCount)
        FileEdit_CommitLine();

    s_editIdx = lineIdx;
    FE_Copy(s_workBuf, s_lines[lineIdx], FE_MAX_LINE_LEN);
    s_caret = FE_Len(s_workBuf);
}

// ============================================================================
// Character operations
// ============================================================================

void FileEdit_InsertChar(char c)
{
    int len = FE_Len(s_workBuf);
    if (len >= FE_MAX_LINE_LEN - 1) return;
    for (int i = len; i >= s_caret; --i)
        s_workBuf[i + 1] = s_workBuf[i];
    s_workBuf[s_caret++] = c;
}

void FileEdit_Backspace()
{
    if (s_caret <= 0) return;
    int len = FE_Len(s_workBuf);
    for (int i = s_caret - 1; i < len; ++i)
        s_workBuf[i] = s_workBuf[i + 1];
    s_caret--;
}

void FileEdit_CaretLeft() { if (s_caret > 0) s_caret--; }
void FileEdit_CaretRight() { int l = FE_Len(s_workBuf); if (s_caret < l) s_caret++; }

// ============================================================================
// FileEdit_CommitLine
// ============================================================================

void FileEdit_CommitLine()
{
    if (s_editIdx < 0 || s_editIdx >= s_lineCount) return;

    // Only mark edited if content actually changed
    const char* orig = s_lines[s_editIdx];
    const char* work = s_workBuf;
    bool changed = false;
    int i = 0;
    while (orig[i] || work[i])
    {
        if (orig[i] != work[i]) { changed = true; break; }
        i++;
    }

    if (changed)
    {
        FE_Copy(s_lines[s_editIdx], s_workBuf, FE_MAX_LINE_LEN);
        s_edited[s_editIdx] = true;
        s_dirty = true;
    }
}

// ============================================================================
// FileEdit_InsertLineAfter
// ============================================================================

int FileEdit_InsertLineAfter(int lineIdx)
{
    FileEdit_CommitLine();
    if (s_lineCount >= FE_MAX_LINES) return s_lineCount;

    int insertAt = lineIdx + 1;
    // Shift everything down
    for (int i = s_lineCount; i > insertAt; --i)
    {
        s_info[i] = s_info[i - 1];
        s_edited[i] = s_edited[i - 1];
        s_deleted[i] = s_deleted[i - 1];
        FE_Copy(s_lines[i], s_lines[i - 1], FE_MAX_LINE_LEN);
    }

    // New inserted line — bufOffset = -1 signals it has no original bytes
    s_info[insertAt].bufOffset = -1;
    s_info[insertAt].contentLen = 0;
    s_info[insertAt].termLen = 0;
    s_lines[insertAt][0] = '\0';
    s_edited[insertAt] = true;
    s_deleted[insertAt] = false;

    s_lineCount++;
    s_dirty = true;

    s_editIdx = insertAt;
    s_workBuf[0] = '\0';
    s_caret = 0;

    return s_lineCount;
}

// ============================================================================
// FileEdit_DeleteLine
// ============================================================================

int FileEdit_DeleteLine(int lineIdx)
{
    if (lineIdx < 0 || lineIdx >= s_lineCount) return s_lineCount;

    // Count non-deleted lines — need at least 2 to allow deletion
    int live = 0;
    for (int i = 0; i < s_lineCount; ++i)
        if (!s_deleted[i]) live++;
    if (live <= 1) return s_lineCount;

    s_deleted[lineIdx] = true;
    s_dirty = true;

    if (s_editIdx == lineIdx)
    {
        s_editIdx = -1;
        s_caret = 0;
        s_workBuf[0] = '\0';
    }

    return s_lineCount;
}

// Returns the logical (non-deleted) line count
static int LiveCount()
{
    int n = 0;
    for (int i = 0; i < s_lineCount; ++i)
        if (!s_deleted[i]) n++;
    return n;
}

// ============================================================================
// FileEdit_Save — byte-perfect write
// ============================================================================

bool FileEdit_Save(const char* path)
{
    if (!path || !path[0]) return false;
    if (LiveCount() == 0)   return false;

    FileEdit_CommitLine();

    HANDLE hFile = CreateFile(path, GENERIC_WRITE, 0,
        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        s_lastError = GetLastError();
        return false;
    }

    bool ok = true;

    for (int i = 0; i < s_lineCount && ok; ++i)
    {
        if (s_deleted[i]) continue;

        DWORD w = 0;

        if (!s_edited[i] && s_info[i].bufOffset >= 0)
        {
            // Unchanged line — copy original bytes verbatim (content + terminator)
            int totalLen = s_info[i].contentLen + s_info[i].termLen;
            if (totalLen > 0)
            {
                if (!WriteFile(hFile,
                    s_rawBuf + s_info[i].bufOffset,
                    (DWORD)totalLen, &w, NULL) || w != (DWORD)totalLen)
                {
                    s_lastError = GetLastError();
                    ok = false;
                }
            }
        }
        else
        {
            // Edited or inserted line — write new content
            int len = FE_Len(s_lines[i]);
            if (len > 0)
            {
                if (!WriteFile(hFile, s_lines[i], (DWORD)len, &w, NULL) || w != (DWORD)len)
                {
                    s_lastError = GetLastError();
                    ok = false;
                }
            }

            // Write original terminator if available, else use default
            if (ok)
            {
                if (s_info[i].bufOffset >= 0 && s_info[i].termLen > 0)
                {
                    // Use this line's original terminator
                    if (!WriteFile(hFile,
                        s_rawBuf + s_info[i].bufOffset + s_info[i].contentLen,
                        (DWORD)s_info[i].termLen, &w, NULL))
                    {
                        s_lastError = GetLastError();
                        ok = false;
                    }
                }
                else if (i < s_lineCount - 1)
                {
                    // Inserted line — use terminator from nearest preceding original line
                    WriteDefaultTerm(hFile, i - 1);
                }
                // Last line with no terminator in original — write nothing
            }
        }
    }

    FlushFileBuffers(hFile);
    CloseHandle(hFile);

    if (ok) { s_wasSaved = true; s_dirty = false; s_lastError = 0; }
    return ok;
}

// ============================================================================
// Query API
// ============================================================================

const char* FileEdit_GetLine(int lineIdx)
{
    if (lineIdx < 0 || lineIdx >= s_lineCount) return "";
    if (s_deleted[lineIdx]) return "";
    if (lineIdx == s_editIdx) return s_workBuf;
    return s_lines[lineIdx];
}

const char* FileEdit_GetWorkBuf() { return s_workBuf; }
int         FileEdit_GetCaret() { return s_caret; }
int         FileEdit_GetEditIdx() { return s_editIdx; }
bool        FileEdit_IsDirty() { return s_dirty; }
bool        FileEdit_WasSaved() { return s_wasSaved; }
DWORD       FileEdit_GetLastError() { return s_lastError; }

int FileEdit_GetCount()
{
    return LiveCount();
}