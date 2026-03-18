// FileViewer.cpp
// XbDiag - Read-only text/CSV file viewer.
// See FileViewer.h for full API and usage notes.
//
// Memory model:
//   s_buf        — malloc'd on open, freed on close (both normal and error paths).
//   s_lines[]    — array of char* pointers into s_buf.
//                  Wrap segments also point into s_buf — no second allocation.
//   s_lineCount  — total virtual lines (including wrap continuations).
//
// Line building:
//   1. Replace every \r\n or \n in s_buf with \0 to split into raw lines.
//   2. For each raw line, measure char count.
//      - If <= CHARS_PER_LINE: add one entry to s_lines[].
//      - If >  CHARS_PER_LINE: add multiple entries, each pointing to the
//        start of its 80-char segment within s_buf. The last segment of a
//        wrapped line gets a WRAP_CONT marker in s_wrapCont[] so the
//        renderer can draw a small continuation indicator.

#include "FileViewer.h"
#include "input.h"
#include <xtl.h>
#include <stdlib.h>   // malloc, free

// ============================================================================
// Layout constants  (640x480 design units)
// ============================================================================

static const float FV_TEXT_SCALE = 1.2f;
static const float FV_LINE_H = LINE_H;          // 18px — matches rest of UI
static const float FV_LIST_Y = CONTENT_Y + 2.f; // top of text area
static const float FV_LIST_BOT = BOT_BAR_Y - 2.f; // bottom of text area
static const float FV_TEXT_X = LM;              // left margin

static const int   ROWS_VIS = 21;   // floor((450-60)/18)
static const int   CHARS_PER_LINE = 80;   // chars that fit at scale 1.2 in 576px

static const int   MAX_LINES = 8192; // max virtual lines (wraps included)
static const DWORD MAX_FILE_BYTES = 512 * 1024;  // 512KB cap

// ============================================================================
// State
// ============================================================================

static bool    s_active = false;
static char* s_buf = NULL;   // malloc'd file buffer
static DWORD   s_bufSize = 0;      // bytes actually read

static char* s_lines[MAX_LINES];    // pointers into s_buf
static bool    s_wrapCont[MAX_LINES]; // true = this line is a wrap continuation
static int     s_lineCount = 0;

static int     s_scroll = 0;      // top visible line index
static WORD    s_prev = 0;

static bool    s_truncated = false;  // file exceeded MAX_FILE_BYTES
static char    s_titleBuf[72];        // "filename.txt  (NNN lines)" shown in top bar

// ============================================================================
// Helpers
// ============================================================================

static bool FV_Edge(WORD cur, WORD btn)
{
    return (cur & btn) && !(s_prev & btn);
}

// Case-insensitive 4-char extension compare
static bool ExtMatch(const char* name, const char* ext)
{
    int nlen = 0; while (name[nlen]) nlen++;
    int elen = 0; while (ext[elen]) elen++;
    if (nlen < elen) return false;
    const char* tail = name + nlen - elen;
    for (int i = 0; i < elen; ++i)
    {
        char a = tail[i]; if (a >= 'A' && a <= 'Z') a = char(a + 32);
        char b = ext[i];  if (b >= 'A' && b <= 'Z') b = char(b + 32);
        if (a != b) return false;
    }
    return true;
}

static void FreeBuffer()
{
    if (s_buf)
    {
        free(s_buf);
        s_buf = NULL;
    }
    s_bufSize = 0;
    s_lineCount = 0;
}

// Build s_lines[] from s_buf.
// Replaces \r and \n in-place with \0 to terminate raw lines,
// then wraps any raw line longer than CHARS_PER_LINE into segments.
static void BuildLines()
{
    s_lineCount = 0;
    if (!s_buf || s_bufSize == 0) return;

    // Null-terminate the buffer
    s_buf[s_bufSize] = '\0';

    char* p = s_buf;
    char* end = s_buf + s_bufSize;

    while (p < end && s_lineCount < MAX_LINES)
    {
        // Find end of raw line
        char* lineStart = p;
        while (p < end && *p != '\r' && *p != '\n') p++;

        // Terminate: replace \r\n, \n, \r
        if (p < end)
        {
            *p = '\0';
            p++;
            if (p < end && *(p - 1) == '\r' && *p == '\n') // handle \r\n
                p++;
        }

        // Measure raw line length
        int rawLen = 0;
        while (lineStart[rawLen]) rawLen++;

        if (rawLen == 0)
        {
            // Empty line
            if (s_lineCount < MAX_LINES)
            {
                s_lines[s_lineCount] = lineStart;
                s_wrapCont[s_lineCount] = false;
                s_lineCount++;
            }
            continue;
        }

        // Wrap into CHARS_PER_LINE segments
        char* seg = lineStart;
        int   remain = rawLen;
        bool  first = true;

        while (remain > 0 && s_lineCount < MAX_LINES)
        {
            s_lines[s_lineCount] = seg;
            s_wrapCont[s_lineCount] = !first;
            s_lineCount++;

            if (remain <= CHARS_PER_LINE)
                break;

            // Insert a temporary \0 at the wrap point so this segment
            // renders as a terminated string. The original char is saved
            // in the next segment's first byte — BUT we can't modify s_buf
            // mid-string without destroying the next segment. Instead we
            // use a different strategy: store the wrap length in a parallel
            // array and truncate rendering by character count, not by \0.
            // (See DrawLine below — it renders exactly s_wrapLen[i] chars.)
            seg += CHARS_PER_LINE;
            remain -= CHARS_PER_LINE;
            first = false;
        }
    }
}

// Render exactly `count` characters from `str` at (x, y).
// Used to render wrap segments that don't have a \0 at the wrap boundary.
static void DrawLineN(float x, float y, const char* str, int count,
    float scale, DWORD color)
{
    // Write a temporary \0, draw, restore — safe because wrap segments
    // are always followed by more buffer content (never at the very end
    // of s_buf, which already has a hard \0 guard).
    // Only do this when count < full length; at end-of-segment str is
    // already \0-terminated by the raw line terminator or buffer guard.
    char saved = str[count];
    ((char*)str)[count] = '\0';
    DrawText(x, y, str, scale, color);
    ((char*)str)[count] = saved;
}

// Returns the rendered length of a virtual line (capped at CHARS_PER_LINE)
static int LineLen(int idx)
{
    if (idx < 0 || idx >= s_lineCount) return 0;
    const char* s = s_lines[idx];
    int n = 0;
    while (s[n] && n < CHARS_PER_LINE) n++;
    return n;
}

// ============================================================================
// FileViewer_CanOpen
// ============================================================================

bool FileViewer_CanOpen(const char* filename)
{
    if (!filename) return false;
    return ExtMatch(filename, ".txt") || ExtMatch(filename, ".csv");
}

// ============================================================================
// FileViewer_Open
// ============================================================================

void FileViewer_Open(const char* path, const char* filename)
{
    // Always free any previous buffer first
    FreeBuffer();

    s_active = false;
    s_scroll = 0;
    s_prev = 0;
    s_truncated = false;
    s_titleBuf[0] = '\0';

    if (!path || !path[0]) return;

    // Open file
    HANDLE hFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0)
    {
        CloseHandle(hFile);
        // Still enter viewer with empty buffer — shows "0 lines"
        s_buf = (char*)malloc(1);
        if (s_buf) { s_buf[0] = '\0'; s_bufSize = 0; }
        BuildLines();
        s_active = true;
        StrCopy(s_titleBuf, sizeof(s_titleBuf), filename ? filename : "FILE VIEWER");
        return;
    }

    // Cap at MAX_FILE_BYTES — allocate one extra byte for the \0 guard
    DWORD readSize = fileSize;
    if (readSize > MAX_FILE_BYTES)
    {
        readSize = MAX_FILE_BYTES;
        s_truncated = true;
    }

    // +1 for null guard after BuildLines
    s_buf = (char*)malloc(readSize + 1);
    if (!s_buf)
    {
        CloseHandle(hFile);
        return;
    }

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, s_buf, readSize, &bytesRead, NULL);
    CloseHandle(hFile);

    if (!ok || bytesRead == 0)
    {
        FreeBuffer();
        return;
    }

    s_bufSize = bytesRead;
    s_buf[s_bufSize] = '\0';  // guard — BuildLines relies on this

    BuildLines();

    // Build title: "filename  (N lines)"
    // Keep it short enough to fit the top bar
    char lineCnt[12];
    IntToStr(s_lineCount, lineCnt, sizeof(lineCnt));
    StrCopy(s_titleBuf, sizeof(s_titleBuf), filename ? filename : "FILE");
    StrCat2(s_titleBuf, sizeof(s_titleBuf), s_titleBuf, "  (");
    StrCat2(s_titleBuf, sizeof(s_titleBuf), s_titleBuf, lineCnt);
    StrCat2(s_titleBuf, sizeof(s_titleBuf), s_titleBuf, " lines)");

    s_active = true;
}

// ============================================================================
// FileViewer_IsActive
// ============================================================================

bool FileViewer_IsActive()
{
    return s_active;
}

// ============================================================================
// FileViewer_Tick  — input + render, call every frame while active
// ============================================================================

void FileViewer_Tick(const DiagLogo& logo)
{
    if (!s_active) return;

    // ── Input ────────────────────────────────────────────────────────────────

    WORD cur = GetButtons();

    // B — exit, free buffer
    if (FV_Edge(cur, BTN_B))
    {
        FreeBuffer();
        s_active = false;
        s_prev = cur;
        return;
    }

    // DPad Down — one line
    if (FV_Edge(cur, BTN_DPAD_DOWN))
    {
        if (s_scroll < s_lineCount - ROWS_VIS)
            s_scroll++;
    }

    // DPad Up — one line
    if (FV_Edge(cur, BTN_DPAD_UP))
    {
        if (s_scroll > 0)
            s_scroll--;
    }

    // RT — page down
    if (FV_Edge(cur, BTN_RTRIG))
    {
        s_scroll += ROWS_VIS;
        int maxScroll = s_lineCount - ROWS_VIS;
        if (maxScroll < 0) maxScroll = 0;
        if (s_scroll > maxScroll) s_scroll = maxScroll;
    }

    // LT — page up
    if (FV_Edge(cur, BTN_LTRIG))
    {
        s_scroll -= ROWS_VIS;
        if (s_scroll < 0) s_scroll = 0;
    }

    s_prev = cur;

    // ── Render ───────────────────────────────────────────────────────────────

    g_pDevice->BeginScene();

    DrawPageChrome(logo, s_titleBuf, "[DPad] Scroll  [LT/RT] Page  [B] Close");

    // Alternate row shading for readability
    for (int i = 0; i < ROWS_VIS; ++i)
    {
        int idx = s_scroll + i;
        if (idx >= s_lineCount) break;

        float ry = FV_LIST_Y + (float)i * FV_LINE_H;

        // Subtle alternate stripe
        if (i & 1)
            FillRect(0.f, ry, SW, ry + FV_LINE_H, D3DCOLOR_XRGB(10, 12, 28));

        const char* linePtr = s_lines[idx];
        int         lineLen = LineLen(idx);
        bool        isCont = s_wrapCont[idx];

        // Wrap continuation indicator — draw a small dim '>' at the margin
        DWORD textCol = COL_WHITE;
        float textX = FV_TEXT_X;

        if (isCont)
        {
            DrawText(textX, ry, " ", FV_TEXT_SCALE, COL_DIM);
            textCol = D3DCOLOR_XRGB(180, 180, 180);  // slightly dimmer for cont lines
            textX = FV_TEXT_X + TW(" ", FV_TEXT_SCALE);
        }

        // Draw the line — use DrawLineN for wrap segments (no trailing \0 at wrap point)
        // Check if this is a mid-wrap segment (next line is also a continuation)
        bool hasMoreWrap = (idx + 1 < s_lineCount) && s_wrapCont[idx + 1];
        bool isMidWrap = hasMoreWrap && (lineLen == CHARS_PER_LINE);

        if (isMidWrap)
            DrawLineN(textX, ry, linePtr, CHARS_PER_LINE, FV_TEXT_SCALE, textCol);
        else
            DrawText(textX, ry, linePtr, FV_TEXT_SCALE, textCol);
    }

    // ── Line number / position indicator (bottom-right of content area) ──────
    {
        int visBot = s_scroll + ROWS_VIS;
        if (visBot > s_lineCount) visBot = s_lineCount;

        // "line X-Y / Z"
        char posA[8], posB[8], posC[8];
        IntToStr(s_scroll + 1, posA, sizeof(posA));
        IntToStr(visBot, posB, sizeof(posB));
        IntToStr(s_lineCount, posC, sizeof(posC));

        char posBuf[32];
        StrCopy(posBuf, sizeof(posBuf), posA);
        StrCat2(posBuf, sizeof(posBuf), posBuf, "-");
        StrCat2(posBuf, sizeof(posBuf), posBuf, posB);
        StrCat2(posBuf, sizeof(posBuf), posBuf, " / ");
        StrCat2(posBuf, sizeof(posBuf), posBuf, posC);

        DrawTextR(SW - LM, FV_LIST_BOT - FV_LINE_H, posBuf, 1.05f, COL_GRAY);
    }

    // ── Scroll bar ────────────────────────────────────────────────────────────
    if (s_lineCount > ROWS_VIS)
    {
        float sbX = SW - LM + 4.f;
        float sbY0 = FV_LIST_Y;
        float sbH = FV_LIST_BOT - FV_LIST_Y;
        float thH = sbH * ((float)ROWS_VIS / (float)s_lineCount);
        if (thH < 6.f) thH = 6.f;
        float thY = sbY0 + (sbH - thH) *
            ((float)s_scroll / (float)(s_lineCount - ROWS_VIS));

        FillRect(sbX, sbY0, sbX + 4.f, sbY0 + sbH, D3DCOLOR_XRGB(18, 22, 48));
        FillRect(sbX, thY, sbX + 4.f, thY + thH, COL_BORDER);
    }

    // ── Truncation warning ────────────────────────────────────────────────────
    if (s_truncated)
    {
        const char* warn = "[ FILE TRUNCATED AT 512KB ]";
        float wx = SW * 0.5f - TW(warn, 1.05f) * 0.5f;
        DrawText(wx, FV_LIST_BOT - FV_LINE_H, warn, 1.05f, COL_ORANGE);
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}