// FileViewer.cpp
// XbDiag - Text/CSV/CFG/INI file viewer with integrated line editor.
// See FileViewer.h for full API and usage notes.
//
// Modes:
//   View mode  — DPad scrolls. [Y] enters edit mode (disabled if truncated).
//   Edit mode  — Keyboard overlay always visible at bottom of screen.
//                Left stick navigates keyboard key selector.
//                DPad Up/Down moves between lines (commits current line first).
//                DPad Left/Right moves caret within current line.
//                [A]     — type selected key at caret.
//                [X]     — backspace at caret.
//                [Y]     — insert space at caret.
//                [L3]    — toggle caps on keyboard.
//                [START] — commit current line and stay in edit mode.
//                [START+X] — delete current line.
//                [BACK] — save confirm box.
//                [B]     — exit edit mode back to view.
//
// Confirm box drawn as inline modal over file content.

#include "FileViewer.h"
#include "FileEdit.h"
#include "Keyboard.h"
#include "input.h"
#include <xtl.h>
#include <stdlib.h>

// ============================================================================
// Layout constants  (640x480 design units)
// ============================================================================

static const float FV_TEXT_SCALE = 1.2f;
static const float FV_LINE_H = LINE_H;
static const float FV_LIST_Y = CONTENT_Y + 2.f;
static const float FV_TEXT_X = LM;

// In edit mode the keyboard overlay covers the bottom portion.
// Reduce visible rows so file content sits above the overlay panel.
// Overlay panel top is at 258px (from Keyboard.cpp OV_PANEL_Y).
static const float FV_LIST_BOT_VIEW = BOT_BAR_Y - 2.f;  // view mode bottom
static const float FV_LIST_BOT_EDIT = 255.f;              // edit mode bottom (above overlay)

static const int   ROWS_VIS_VIEW = 21;
static const int   ROWS_VIS_EDIT = 11;  // rows visible above keyboard overlay

static const int   CHARS_PER_LINE = 80;
static const int   MAX_LINES = 8192;
static const DWORD MAX_FILE_BYTES = 512 * 1024;

// Stick repeat — frames before first repeat, then repeat interval
static const int STICK_DELAY = 18;
static const int STICK_REPEAT = 5;

// ============================================================================
// State
// ============================================================================

static bool   s_active = false;
static char* s_buf = NULL;      // working buffer — BuildLines writes \0 into this
static char* s_rawBuf = NULL;      // clean copy — never modified, used by FileEdit_Save
static DWORD  s_bufSize = 0;

static char* s_lines[MAX_LINES];
static bool   s_wrapCont[MAX_LINES];
static int    s_lineCount = 0;

// Per-logical-line info captured before BuildLines nulls terminators
static FE_LineInfo s_lineInfo[MAX_LINES];
static int         s_logicalLineCount = 0;

static int    s_scroll = 0;
static WORD   s_prev = 0;

static bool   s_truncated = false;
static char   s_titleBuf[72];
static char   s_filePath[256];
static char   s_fileName[72];

// Edit mode
static bool   s_editMode = false;  // keyboard overlay + cursor active
static bool   s_editLoaded = false;  // FileEdit table has data (survives keyboard dismiss)
static int    s_cursor = 0;    // selected line in edit table
static bool   s_confirmOpen = false;
static bool   s_confirmReady = false;  // true once A is released after confirm opens
static int    s_blinkTimer = 0;
static bool   s_saveError = false;
static int    s_saveErrTimer = 0;

// Stick navigation state for keyboard key selector
static int    s_stickHoldX = 0;   // frames stick held in X
static int    s_stickHoldY = 0;   // frames stick held in Y

static const DiagLogo* s_logo = NULL;

// Forward declarations
static void RenderFileViewer();
static void DrawConfirmBox();
static void DrawHelperBar();

// ============================================================================
// Helpers
// ============================================================================

static bool FV_Edge(WORD cur, WORD btn)
{
    return (cur & btn) && !(s_prev & btn);
}

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
    if (s_buf) { free(s_buf);    s_buf = NULL; }
    if (s_rawBuf) { free(s_rawBuf); s_rawBuf = NULL; }
    s_bufSize = 0;
    s_lineCount = 0;
}

// Capture per-logical-line byte offsets and terminator info from the raw buffer.
// Must be called BEFORE BuildLines() which nulls out terminators in place.
static void BuildLineInfo()
{
    s_logicalLineCount = 0;
    if (!s_buf || s_bufSize == 0) return;

    const char* p = s_buf;
    const char* end = s_buf + s_bufSize;

    while (p < end && s_logicalLineCount < MAX_LINES)
    {
        const char* lineStart = p;
        int offset = (int)(p - s_buf);

        // Scan to end of line content
        while (p < end && *p != '\r' && *p != '\n') p++;
        int contentLen = (int)(p - lineStart);

        // Measure terminator
        int termLen = 0;
        if (p < end && *p == '\r') { termLen++; p++; }
        if (p < end && *p == '\n') { termLen++; p++; }

        FE_LineInfo& li = s_lineInfo[s_logicalLineCount++];
        li.bufOffset = offset;
        li.contentLen = contentLen;
        li.termLen = termLen;
    }
}

static void BuildLines()
{
    s_lineCount = 0;
    if (!s_buf || s_bufSize == 0) return;
    s_buf[s_bufSize] = '\0';
    char* p = s_buf;
    char* end = s_buf + s_bufSize;
    while (p < end && s_lineCount < MAX_LINES)
    {
        char* lineStart = p;
        while (p < end && *p != '\r' && *p != '\n') p++;
        if (p < end)
        {
            *p = '\0'; p++;
            if (p < end && *(p - 1) == '\r' && *p == '\n') p++;
        }
        int rawLen = 0; while (lineStart[rawLen]) rawLen++;
        if (rawLen == 0)
        {
            if (s_lineCount < MAX_LINES)
            {
                s_lines[s_lineCount] = lineStart;
                s_wrapCont[s_lineCount] = false;
                s_lineCount++;
            }
            continue;
        }
        char* seg = lineStart; int remain = rawLen; bool first = true;
        while (remain > 0 && s_lineCount < MAX_LINES)
        {
            s_lines[s_lineCount] = seg;
            s_wrapCont[s_lineCount] = !first;
            s_lineCount++;
            if (remain <= CHARS_PER_LINE) break;
            seg += CHARS_PER_LINE; remain -= CHARS_PER_LINE; first = false;
        }
    }
}

static void DrawLineN(float x, float y, const char* str, int count,
    float scale, DWORD color)
{
    char saved = str[count];
    ((char*)str)[count] = '\0';
    DrawText(x, y, str, scale, color);
    ((char*)str)[count] = saved;
}

static int LineLen(int idx)
{
    if (idx < 0 || idx >= s_lineCount) return 0;
    const char* s = s_lines[idx]; int n = 0;
    while (s[n] && n < CHARS_PER_LINE) n++; return n;
}

static int DispCount()
{
    return s_editLoaded ? FileEdit_GetCount() : s_lineCount;
}

static void ClampScroll()
{
    int rowsVis = s_editMode ? ROWS_VIS_EDIT : ROWS_VIS_VIEW;
    int count = DispCount();
    if (s_editMode)
    {
        if (s_cursor < s_scroll) s_scroll = s_cursor;
        if (s_cursor >= s_scroll + rowsVis) s_scroll = s_cursor - rowsVis + 1;
    }
    if (s_scroll < 0) s_scroll = 0;
    int maxScroll = count - rowsVis;
    if (maxScroll < 0) maxScroll = 0;
    if (s_scroll > maxScroll) s_scroll = maxScroll;
}

// Move cursor to a new line — commits current line first
static void MoveCursorTo(int newLine)
{
    FileEdit_CommitLine();
    int count = FileEdit_GetCount();
    if (newLine < 0) newLine = 0;
    if (newLine >= count) newLine = count - 1;
    s_cursor = newLine;
    ClampScroll();
    FileEdit_SetLine(s_cursor);
}

// ============================================================================
// FileViewer_CanOpen
// ============================================================================

bool FileViewer_CanOpen(const char* filename)
{
    if (!filename) return false;
    return ExtMatch(filename, ".txt") || ExtMatch(filename, ".csv")
        || ExtMatch(filename, ".cfg") || ExtMatch(filename, ".ini");
}

// ============================================================================
// FileViewer_Open
// ============================================================================

void FileViewer_Open(const char* path, const char* filename)
{
    FreeBuffer();
    s_active = false;
    s_scroll = 0;
    s_prev = 0;
    s_truncated = false;
    s_editMode = false;
    s_editLoaded = false;
    s_cursor = 0;
    s_confirmOpen = false;
    s_confirmReady = false;
    s_blinkTimer = 0;
    s_saveError = false;
    s_saveErrTimer = 0;
    s_stickHoldX = 0;
    s_stickHoldY = 0;
    s_titleBuf[0] = '\0';
    s_filePath[0] = '\0';
    s_fileName[0] = '\0';

    if (!path || !path[0]) return;
    StrCopy(s_filePath, sizeof(s_filePath), path);
    StrCopy(s_fileName, sizeof(s_fileName), filename ? filename : "");

    HANDLE hFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0)
    {
        CloseHandle(hFile);
        s_buf = (char*)malloc(1);
        if (s_buf) { s_buf[0] = '\0'; s_bufSize = 0; }
        BuildLines();
        s_active = true;
        StrCopy(s_titleBuf, sizeof(s_titleBuf), filename ? filename : "FILE VIEWER");
        return;
    }

    DWORD readSize = fileSize;
    if (readSize > MAX_FILE_BYTES) { readSize = MAX_FILE_BYTES; s_truncated = true; }
    s_buf = (char*)malloc(readSize + 1);
    if (!s_buf) { CloseHandle(hFile); return; }

    DWORD bytesRead = 0;
    BOOL  ok = ReadFile(hFile, s_buf, readSize, &bytesRead, NULL);
    CloseHandle(hFile);
    if (!ok || bytesRead == 0) { FreeBuffer(); return; }

    s_bufSize = bytesRead;
    s_buf[s_bufSize] = '\0';

    // Make a clean copy of the raw bytes BEFORE BuildLines nulls terminators.
    // FileEdit_Save reads from s_rawBuf to write unchanged lines verbatim.
    s_rawBuf = (char*)malloc(bytesRead);
    if (s_rawBuf)
    {
        for (DWORD i = 0; i < bytesRead; ++i)
            s_rawBuf[i] = s_buf[i];
    }

    BuildLineInfo();   // capture offsets — must run before BuildLines
    BuildLines();      // nulls terminators in s_buf (s_rawBuf stays clean)

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

bool FileViewer_IsActive() { return s_active; }

// ============================================================================
// FileViewer_Tick
// ============================================================================

void FileViewer_Tick(const DiagLogo& logo)
{
    if (!s_active) return;
    s_logo = &logo;
    s_blinkTimer++;
    if (s_saveErrTimer > 0) s_saveErrTimer--;

    WORD cur = GetButtons();

    // ── Confirm box ───────────────────────────────────────────────────────────
    if (s_confirmOpen)
    {
        // Wait until A is fully released before accepting it as a confirm press
        if (!s_confirmReady && !(cur & BTN_A))
            s_confirmReady = true;

        if (s_confirmReady && FV_Edge(cur, BTN_A))
        {
            s_confirmOpen = false;
            s_confirmReady = false;
            bool ok = FileEdit_Save(s_filePath);
            if (ok)
            {
                FileEdit_Unload();
                s_editMode = false;
                s_editLoaded = false;
                s_prev = cur;
                FileViewer_Open(s_filePath, s_fileName);
                return;
            }
            else
            {
                s_saveError = true;
                s_saveErrTimer = 180;
            }
        }
        else if (FV_Edge(cur, BTN_B))
        {
            s_confirmOpen = false;
            s_confirmReady = false;
        }
        s_prev = cur;
        g_pDevice->BeginScene();
        RenderFileViewer();
        Keyboard_RenderOverlay();   // keyboard still visible behind confirm box
        DrawConfirmBox();
        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
        return;
    }

    // ── Edit mode ─────────────────────────────────────────────────────────────
    if (s_editMode)
    {
        int lx, ly, rx, ry;
        GetSticks(lx, ly, rx, ry);

        // ── Left stick drives keyboard key selector (with repeat) ─────────────
        // Horizontal
        {
            int dir = 0;
            if (lx > 10000)       dir = +1;
            else if (lx < -10000) dir = -1;

            if (dir == 0)
            {
                s_stickHoldX = 0;
            }
            else
            {
                s_stickHoldX++;
                bool fire = (s_stickHoldX == 1) ||
                    (s_stickHoldX > STICK_DELAY &&
                        (s_stickHoldX - STICK_DELAY) % STICK_REPEAT == 0);
                if (fire)
                    Keyboard_StepCol(dir);
            }
        }
        // Vertical (stick Y: up = positive on Xbox)
        {
            int dir = 0;
            if (ly > 10000)       dir = -1;  // stick up = row up
            else if (ly < -10000) dir = +1;

            if (dir == 0)
            {
                s_stickHoldY = 0;
            }
            else
            {
                s_stickHoldY++;
                bool fire = (s_stickHoldY == 1) ||
                    (s_stickHoldY > STICK_DELAY &&
                        (s_stickHoldY - STICK_DELAY) % STICK_REPEAT == 0);
                if (fire)
                    Keyboard_StepRow(dir);
            }
        }

        // ── B — dismiss keyboard/cursor, keep edits alive for later save ────────
        if (FV_Edge(cur, BTN_B))
        {
            FileEdit_CommitLine();
            s_editMode = false;   // hide keyboard overlay
            // s_editLoaded stays true — edits persist, user can still save
            s_blinkTimer = 0;
            s_prev = cur;
            g_pDevice->BeginScene();
            RenderFileViewer();
            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);
            return;
        }

        // ── BACK — save confirm ────────────────────────────────────────────
        // Edge on START (not A) — A is frequently in s_prev from typing so
        // FV_Edge on A would miss the combo. START is only used for combos
        // so its edge is clean.
        if (FV_Edge(cur, BTN_BACK))
        {
            s_confirmOpen = true; s_confirmReady = false;
            s_prev = cur;
            g_pDevice->BeginScene();
            RenderFileViewer();
            Keyboard_RenderOverlay();
            DrawConfirmBox();
            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);
            return;
        }

        // ── START+X — delete current line ────────────────────────────────────
        if (FV_Edge(cur, BTN_START) && (cur & BTN_X))
        {
            int newCount = FileEdit_DeleteLine(s_cursor);
            if (s_cursor >= newCount && newCount > 0) s_cursor = newCount - 1;
            ClampScroll();
            if (newCount > 0) FileEdit_SetLine(s_cursor);
            s_prev = cur;
            g_pDevice->BeginScene();
            RenderFileViewer();
            Keyboard_RenderOverlay();
            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);
            return;
        }

        // ── START — commit current line ───────────────────────────────────────
        if (FV_Edge(cur, BTN_START) && !(cur & BTN_X))
        {
            FileEdit_CommitLine();
            s_prev = cur;
            g_pDevice->BeginScene();
            RenderFileViewer();
            Keyboard_RenderOverlay();
            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);
            return;
        }

        // ── A — type selected keyboard key at caret ───────────────────────────
        if (FV_Edge(cur, BTN_A) && !(cur & BTN_START))
        {
            char c = Keyboard_GetSelectedChar();
            if (c)
            {
                FileEdit_InsertChar(c);
            }
            else
            {
                int fnKey = Keyboard_GetSelectedFnKey();
                if (fnKey == FK_CAPS)
                    Keyboard_ToggleCaps();
                else if (fnKey == FK_SPACE)
                    FileEdit_InsertChar(' ');
                else if (fnKey == FK_BKSP)
                    FileEdit_Backspace();
                else if (fnKey == FK_CR)
                {
                    FileEdit_InsertLineAfter(s_cursor);
                    s_cursor++;
                    ClampScroll();
                }
                else if (fnKey == FK_DONE)
                {
                    s_confirmOpen = true;
                    s_confirmReady = false;
                }
            }
        }

        // ── X — backspace at caret ────────────────────────────────────────────
        if (FV_Edge(cur, BTN_X) && !(cur & BTN_START))
            FileEdit_Backspace();

        // ── Y — space at caret ────────────────────────────────────────────────
        if (FV_Edge(cur, BTN_Y))
            FileEdit_InsertChar(' ');

        // ── L3 — toggle caps ──────────────────────────────────────────────────
        if (FV_Edge(cur, BTN_LTHUMB))
            Keyboard_ToggleCaps();

        // ── DPad Left/Right — move caret within line ──────────────────────────
        if (FV_Edge(cur, BTN_DPAD_LEFT))  FileEdit_CaretLeft();
        if (FV_Edge(cur, BTN_DPAD_RIGHT)) FileEdit_CaretRight();

        // ── DPad Up/Down — move between lines (commits current first) ─────────
        if (FV_Edge(cur, BTN_DPAD_UP) && s_cursor > 0)
            MoveCursorTo(s_cursor - 1);
        if (FV_Edge(cur, BTN_DPAD_DOWN) && s_cursor < FileEdit_GetCount() - 1)
            MoveCursorTo(s_cursor + 1);

        // ── LT/RT — page up/down ──────────────────────────────────────────────
        if (FV_Edge(cur, BTN_LTRIG)) MoveCursorTo(s_cursor - ROWS_VIS_EDIT);
        if (FV_Edge(cur, BTN_RTRIG)) MoveCursorTo(s_cursor + ROWS_VIS_EDIT);

        s_prev = cur;
        g_pDevice->BeginScene();
        RenderFileViewer();
        Keyboard_RenderOverlay();
        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
        return;
    }

    // ── View mode ─────────────────────────────────────────────────────────────

    // BACK — save from view mode when edits are pending
    if (s_editLoaded && FV_Edge(cur, BTN_BACK))
    {
        s_confirmOpen = true; s_confirmReady = false;
        s_prev = cur;
        g_pDevice->BeginScene();
        RenderFileViewer();
        DrawConfirmBox();
        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
        return;
    }

    if (FV_Edge(cur, BTN_B))
    {
        // Unload any pending edits cleanly before closing
        if (s_editLoaded) FileEdit_Unload();
        s_editLoaded = false;
        FreeBuffer();
        s_active = false;
        s_prev = cur;
        return;
    }

    if (FV_Edge(cur, BTN_Y) && !s_truncated)
    {
        if (!s_editLoaded)
        {
            // Fresh load from viewer buffer
            FileEdit_Load(s_rawBuf, (int)s_bufSize, s_lineInfo, s_logicalLineCount);
            s_editLoaded = true;
            s_cursor = s_scroll;
        }
        // Re-enter edit mode (keyboard overlay back up)
        s_editMode = true;
        s_blinkTimer = 0;
        ClampScroll();
        FileEdit_SetLine(s_cursor);
    }

    if (!s_editMode)
    {
        if (FV_Edge(cur, BTN_DPAD_DOWN))
        {
            if (s_scroll < s_lineCount - ROWS_VIS_VIEW) s_scroll++;
        }
        if (FV_Edge(cur, BTN_DPAD_UP))
        {
            if (s_scroll > 0) s_scroll--;
        }
        if (FV_Edge(cur, BTN_RTRIG))
        {
            s_scroll += ROWS_VIS_VIEW;
            int mx = s_lineCount - ROWS_VIS_VIEW;
            if (mx < 0) mx = 0;
            if (s_scroll > mx) s_scroll = mx;
        }
        if (FV_Edge(cur, BTN_LTRIG))
        {
            s_scroll -= ROWS_VIS_VIEW;
            if (s_scroll < 0) s_scroll = 0;
        }
    }

    s_prev = cur;
    g_pDevice->BeginScene();
    RenderFileViewer();
    if (s_editMode) Keyboard_RenderOverlay();
    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// DrawConfirmBox
// ============================================================================

static void DrawConfirmBox()
{
    const float BW = 340.f, BH = 76.f;
    const float BX = (640.f - BW) * 0.5f;
    const float BY = (FV_LIST_BOT_EDIT - BH) * 0.5f + 58.f;

    FillRect(BX, BY, BX + BW, BY + BH, D3DCOLOR_XRGB(12, 16, 40));
    HLine(BY, BX, BX + BW, COL_YELLOW);
    HLine(BY + BH, BX, BX + BW, COL_YELLOW);
    VLine(BX, BY, BY + BH, COL_BORDER);
    VLine(BX + BW, BY, BY + BH, COL_BORDER);

    const char* hdr = "OVERWRITE FILE?";
    DrawText(BX + (BW - TW(hdr, 1.3f)) * 0.5f, BY + 7.f, hdr, 1.3f, COL_YELLOW);
    DrawText(BX + (BW - TW(s_fileName, 1.1f)) * 0.5f, BY + 26.f, s_fileName, 1.1f, COL_WHITE);
    const char* hint = "[A] Overwrite    [B] Cancel";
    DrawText(BX + (BW - TW(hint, 1.05f)) * 0.5f, BY + 50.f, hint, 1.05f, COL_GRAY);
}

// ============================================================================
// DrawHelperBar  — control hint drawn just above the keyboard overlay
// ============================================================================

static void DrawHelperBar()
{
    const float HY = 258.f - 14.f;  // just above overlay panel top
    FillRect(0.f, HY, 640.f, 258.f, D3DCOLOR_XRGB(6, 8, 20));
    HLine(HY, 0.f, 640.f, COL_BORDER);
    const char* help =
        "[Stick] Key  [DPad LR] Caret  [DPad UD] Line  [A] Type  [X] Bksp  [Y] Space  [Enter] Commit  [BACK] Save  [B] Exit";
    DrawText((640.f - TW(help, 0.9f)) * 0.5f, HY + 2.f, help, 0.9f, COL_DIM);
}

// ============================================================================
// RenderFileViewer
// ============================================================================

static void RenderFileViewer()
{
    // ── Hint bar ──────────────────────────────────────────────────────────────
    const char* hint;
    if (s_editMode)
        hint = FileEdit_IsDirty()
        ? "[BACK] Save*  [START+X] Del  [B] Dismiss kbd"
        : "[BACK] Save  [START+X] Del  [B] Dismiss kbd";
    else if (s_editLoaded && FileEdit_IsDirty())
        hint = "[Y] Resume edit  [BACK] Save*  [B] Close";
    else if (s_editLoaded)
        hint = "[Y] Resume edit  [BACK] Save  [B] Close";
    else if (s_truncated)
        hint = "[DPad] Scroll  [LT/RT] Page  [B] Close  (edit disabled)";
    else
        hint = "[DPad] Scroll  [LT/RT] Page  [Y] Edit  [B] Close";

    DrawPageChrome(*s_logo, s_titleBuf, hint);

    // ── File content ──────────────────────────────────────────────────────────
    int   dispCount = DispCount();
    int   rowsVis = s_editMode ? ROWS_VIS_EDIT : ROWS_VIS_VIEW;
    float listBot = s_editMode ? FV_LIST_BOT_EDIT : FV_LIST_BOT_VIEW;

    for (int i = 0; i < rowsVis; ++i)
    {
        int   idx = s_scroll + i;
        if (idx >= dispCount) break;

        float ry = FV_LIST_Y + (float)i * FV_LINE_H;
        bool  sel = s_editMode && (idx == s_cursor);

        // Row background
        if (sel)
            FillRect(0.f, ry, SW, ry + FV_LINE_H, D3DCOLOR_XRGB(18, 35, 70));
        else if (i & 1)
            FillRect(0.f, ry, SW, ry + FV_LINE_H, D3DCOLOR_XRGB(10, 12, 28));

        if (s_editMode)
        {
            const char* lineText = FileEdit_GetLine(idx);
            DWORD tc = sel ? COL_WHITE : D3DCOLOR_XRGB(180, 200, 220);
            float tx = FV_TEXT_X;

            if (sel)
            {
                // Draw text up to caret, then blinking caret bar, then rest
                int   caret = FileEdit_GetCaret();
                bool  caretOn = (s_blinkTimer % 60) < 30;

                if (caret > 0)
                {
                    char saved = ((char*)lineText)[caret];
                    ((char*)lineText)[caret] = '\0';
                    DrawText(tx, ry, lineText, FV_TEXT_SCALE, tc);
                    float w = TW(lineText, FV_TEXT_SCALE);
                    ((char*)lineText)[caret] = saved;
                    tx += w;
                }

                if (caretOn)
                    FillRect(tx, ry + 1.f, tx + 2.f, ry + FV_LINE_H - 1.f, COL_CYAN);

                if (lineText[caret])
                    DrawText(tx + 3.f, ry, lineText + caret, FV_TEXT_SCALE, tc);
            }
            else
            {
                DrawText(tx, ry, lineText, FV_TEXT_SCALE, tc);
            }
        }
        else if (s_editLoaded)
        {
            // Keyboard dismissed but edits still live — show edited content
            const char* lineText = FileEdit_GetLine(idx);
            DrawText(FV_TEXT_X, ry, lineText, FV_TEXT_SCALE, COL_WHITE);
        }
        else
        {
            // View mode
            bool  isCont = (idx < s_lineCount) ? s_wrapCont[idx] : false;
            DWORD textCol = COL_WHITE;
            float textX = FV_TEXT_X;

            if (isCont)
            {
                DrawText(textX, ry, " ", FV_TEXT_SCALE, COL_DIM);
                textCol = D3DCOLOR_XRGB(180, 180, 180);
                textX = FV_TEXT_X + TW(" ", FV_TEXT_SCALE);
            }

            if (idx < s_lineCount)
            {
                const char* lp = s_lines[idx];
                int         ll = LineLen(idx);
                bool hasMore = (idx + 1 < s_lineCount) && s_wrapCont[idx + 1];
                bool isMidWrap = hasMore && (ll == CHARS_PER_LINE);
                if (isMidWrap)
                    DrawLineN(textX, ry, lp, CHARS_PER_LINE, FV_TEXT_SCALE, textCol);
                else
                    DrawText(textX, ry, lp, FV_TEXT_SCALE, textCol);
            }
        }
    }

    // ── Position indicator ────────────────────────────────────────────────────
    {
        int visBot = s_scroll + rowsVis;
        if (visBot > dispCount) visBot = dispCount;
        char pa[8], pb[8], pc[8];
        IntToStr(s_scroll + 1, pa, sizeof(pa));
        IntToStr(visBot, pb, sizeof(pb));
        IntToStr(dispCount, pc, sizeof(pc));
        char posBuf[32];
        StrCopy(posBuf, sizeof(posBuf), pa);
        StrCat2(posBuf, sizeof(posBuf), posBuf, "-");
        StrCat2(posBuf, sizeof(posBuf), posBuf, pb);
        StrCat2(posBuf, sizeof(posBuf), posBuf, " / ");
        StrCat2(posBuf, sizeof(posBuf), posBuf, pc);

        if (s_editMode)
        {
            char curStr[8]; IntToStr(s_cursor + 1, curStr, sizeof(curStr));
            char curBuf[16]; StrCopy(curBuf, sizeof(curBuf), "L");
            StrCat2(curBuf, sizeof(curBuf), curBuf, curStr);
            DrawText(LM, listBot - FV_LINE_H, curBuf, 1.05f, COL_CYAN);
        }

        if (s_editLoaded && FileEdit_IsDirty())
        {
            const char* dirty = "[ UNSAVED ]";
            DrawText(SW * 0.5f - TW(dirty, 1.0f) * 0.5f,
                listBot - FV_LINE_H, dirty, 1.0f, COL_YELLOW);
        }

        // Save error flash
        if (s_saveErrTimer > 0)
        {
            char errBuf[64];
            char errCode[12];
            IntToStr((int)FileEdit_GetLastError(), errCode, sizeof(errCode));
            StrCopy(errBuf, sizeof(errBuf), "[ SAVE FAILED  ERR:");
            StrCat2(errBuf, sizeof(errBuf), errBuf, errCode);
            StrCat2(errBuf, sizeof(errBuf), errBuf, " ]");
            DrawText(SW * 0.5f - TW(errBuf, 1.0f) * 0.5f,
                listBot - FV_LINE_H * 2.f, errBuf, 1.0f, COL_RED);
        }

        DrawTextR(SW - LM, listBot - FV_LINE_H, posBuf, 1.05f, COL_GRAY);
    }

    // ── Scroll bar ────────────────────────────────────────────────────────────
    if (dispCount > rowsVis)
    {
        float sbX = SW - LM + 4.f;
        float sbY0 = FV_LIST_Y;
        float sbH = listBot - FV_LIST_Y;
        float thH = sbH * ((float)rowsVis / (float)dispCount);
        if (thH < 6.f) thH = 6.f;
        float thY = sbY0 + (sbH - thH) *
            ((float)s_scroll / (float)(dispCount - rowsVis));
        FillRect(sbX, sbY0, sbX + 4.f, sbY0 + sbH, D3DCOLOR_XRGB(18, 22, 48));
        FillRect(sbX, thY, sbX + 4.f, thY + thH, COL_BORDER);
    }

    // ── Truncation warning ────────────────────────────────────────────────────
    if (s_truncated)
    {
        const char* warn = "[ FILE TRUNCATED — EDITING DISABLED ]";
        DrawText(SW * 0.5f - TW(warn, 1.05f) * 0.5f,
            FV_LIST_BOT_VIEW - FV_LINE_H, warn, 1.05f, COL_ORANGE);
    }

    // ── Helper bar (edit mode only, sits just above keyboard overlay) ─────────
    if (s_editMode)
        DrawHelperBar();
}