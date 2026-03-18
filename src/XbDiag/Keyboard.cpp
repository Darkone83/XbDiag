// Keyboard.cpp
// XbDiag virtual on-screen keyboard.
// See Keyboard.h for full API and usage notes.

#include "Keyboard.h"
#include "input.h"
#include "font.h"

// ============================================================================
// Key layout tables
// ============================================================================

// Each row is a null-terminated string of characters.
// Row 5 (function row) is handled separately — not in these tables.

static const char* k_rowsLower[5] =
{
    "1234567890",   // row 0 — numbers
    "qwertyuiop",   // row 1
    "asdfghjkl",    // row 2
    "zxcvbnm",      // row 3
    ".-_@/\\,;",    // row 4 — symbols (8 keys, same in both layers)
};

static const char* k_rowsUpper[5] =
{
    "!@#$%-+=()",   // row 0 — symbols (caps layer)
    "QWERTYUIOP",   // row 1
    "ASDFGHJKL",    // row 2
    "ZXCVBNM",      // row 3
    ".-_@/\\,;",    // row 4 — unchanged
};

// Row widths (number of typable keys per row)
static const int k_rowLen[5] = { 10, 10, 9, 7, 8 };

// Function row key indices (row 5)
enum FnKey { FK_CAPS = 0, FK_SPACE, FK_BKSP, FK_DONE, FK_COUNT };
static const int k_fnRowLen = FK_COUNT;  // 4 keys

static const int k_totalRows = 6;  // rows 0-4 + function row 5

// ============================================================================
// Layout geometry  (640x480 design units)
// ============================================================================

// Key cell size — slightly wider than the standard BW/BH to give letters
// more breathing room given how many keys are on screen at once.
static const float KW = 44.f;   // key width
static const float KH = 28.f;   // key height
static const float KG = 5.f;    // gap between keys

// Vertical start of keyboard grid (below preview strip)
static const float KB_TOP = 140.f;
static const float ROW_STEP = KH + KG;

// Row indent offsets so shorter rows look centred
// Row 0: 10 keys — no indent (full width baseline)
// Row 1: 10 keys — no indent
// Row 2:  9 keys — half key + gap indent
// Row 3:  7 keys — ~1.5 key indent
// Row 4:  8 keys — half key indent
// Row 5: function row — computed separately
static const float k_rowIndent[5] =
{
    0.f,                        // row 0 — 10 keys
    0.f,                        // row 1 — 10 keys
    (KW + KG) * 0.5f,           // row 2 —  9 keys
    (KW + KG) * 1.5f,           // row 3 —  7 keys
    (KW + KG) * 0.5f,           // row 4 —  8 keys
};

// Total width of a 10-key row — used to centre the whole grid
static const float k_gridW = KW * 10.f + KG * 9.f;
static const float k_gridX = (640.f - k_gridW) * 0.5f;  // left edge of widest row

// Preview strip geometry
static const float PREV_Y = CONTENT_Y + 6.f;   // top of preview box
static const float PREV_H = 36.f;               // height
static const float PREV_X0 = 40.f;
static const float PREV_X1 = 600.f;

// ============================================================================
// State
// ============================================================================

static bool             s_active = false;
static char* s_buf = 0;
static int              s_maxLen = 0;
static int              s_len = 0;
static KeyboardDoneFn   s_onDone = 0;
static KeyboardCancelFn s_onCancel = 0;
static const char* s_title = 0;

static bool  s_caps = false;
static int   s_row = 1;   // start on letter row
static int   s_col = 0;
static WORD  s_prev = 0;
static int   s_blink = 0;   // frame counter for cursor blink

// ============================================================================
// Helpers
// ============================================================================

static bool KBEdge(WORD cur, WORD btn)
{
    return (cur & btn) && !(s_prev & btn);
}

// Returns the number of keys in a given row (including function row)
static int RowLen(int row)
{
    if (row < 5) return k_rowLen[row];
    return k_fnRowLen;
}

// Returns the character for a typable key (rows 0-4)
static char KeyChar(int row, int col)
{
    if (row < 0 || row > 4) return 0;
    const char* tbl = s_caps ? k_rowsUpper[row] : k_rowsLower[row];
    int len = k_rowLen[row];
    if (col < 0 || col >= len) return 0;
    return tbl[col];
}

// Returns the X position (left edge) of key [row][col]
static float KeyX(int row, int col)
{
    if (row == 5)
    {
        // Function row: CAPS, SPACE, BKSP, DONE
        // CAPS and DONE are narrow, SPACE is wide
        // Total width matches the grid width
        // Widths: CAPS=60, SPACE=240, BKSP=60, DONE=60, gaps=KG*3
        // Total = 60+240+60+60 + KG*3 = 420+15 = 435 — centre it
        static const float FW_CAPS = 60.f;
        static const float FW_SPACE = 240.f;
        static const float FW_BKSP = 60.f;
        static const float FW_DONE = 80.f;
        static const float FW_TOTAL = FW_CAPS + FW_SPACE + FW_BKSP + FW_DONE + KG * 3.f;
        float fx = (640.f - FW_TOTAL) * 0.5f;
        if (col == FK_CAPS)  return fx;
        fx += FW_CAPS + KG;
        if (col == FK_SPACE) return fx;
        fx += FW_SPACE + KG;
        if (col == FK_BKSP)  return fx;
        fx += FW_BKSP + KG;
        return fx;  // FK_DONE
    }
    return k_gridX + k_rowIndent[row] + col * (KW + KG);
}

// Returns the width of a key
static float KeyW(int row, int col)
{
    if (row != 5) return KW;
    static const float FW_CAPS = 60.f;
    static const float FW_SPACE = 240.f;
    static const float FW_BKSP = 60.f;
    static const float FW_DONE = 80.f;
    if (col == FK_CAPS)  return FW_CAPS;
    if (col == FK_SPACE) return FW_SPACE;
    if (col == FK_BKSP)  return FW_BKSP;
    return FW_DONE;
}

// Returns the Y position (top edge) of a row
static float RowY(int row)
{
    return KB_TOP + row * ROW_STEP;
}

// ============================================================================
// Keyboard_Open
// ============================================================================

void Keyboard_Open(const char* title,
    char* buf,
    int              maxLen,
    KeyboardDoneFn   onDone,
    KeyboardCancelFn onCancel)
{
    s_title = title;
    s_buf = buf;
    s_maxLen = maxLen;
    s_onDone = onDone;
    s_onCancel = onCancel;

    s_len = 0;
    s_caps = false;
    s_row = 1;
    s_col = 0;
    s_prev = 0;
    s_blink = 0;
    s_active = true;

    if (s_buf && s_maxLen > 0)
        s_buf[0] = '\0';
}

// ============================================================================
// Keyboard_IsActive
// ============================================================================

bool Keyboard_IsActive()
{
    return s_active;
}

// ============================================================================
// Render helpers
// ============================================================================

// Draw a single key box
static void DrawKey(int row, int col, bool selected)
{
    float x0 = KeyX(row, col);
    float y0 = RowY(row);
    float kw = KeyW(row, col);
    float x1 = x0 + kw;
    float y1 = y0 + KH;

    // Special colour for CAPS when active
    bool capsActive = (row == 5 && col == FK_CAPS && s_caps);

    DWORD fill, border, tc;
    if (selected)
    {
        fill = D3DCOLOR_XRGB(30, 80, 160);
        border = D3DCOLOR_XRGB(80, 180, 255);
        tc = COL_WHITE;
    }
    else if (capsActive)
    {
        fill = D3DCOLOR_XRGB(20, 80, 30);
        border = D3DCOLOR_XRGB(80, 220, 100);
        tc = COL_GREEN;
    }
    else
    {
        fill = D3DCOLOR_XRGB(14, 20, 48);
        border = COL_BORDER;
        tc = COL_GRAY;
    }

    FillRect(x0, y0, x1, y1, fill);
    HLine(y0, x0, x1, border);
    HLine(y1, x0, x1, border);
    VLine(x0, y0, y1, border);
    VLine(x1, y0, y1, border);

    // Key label
    const char* lbl = 0;
    char charBuf[2] = { 0, 0 };

    if (row == 5)
    {
        if (col == FK_CAPS)  lbl = s_caps ? "CAPS" : "caps";
        if (col == FK_SPACE) lbl = "SPACE";
        if (col == FK_BKSP)  lbl = "BKSP";
        if (col == FK_DONE)  lbl = "DONE";
    }
    else
    {
        charBuf[0] = KeyChar(row, col);
        lbl = charBuf;
    }

    if (lbl && lbl[0])
    {
        float ts = 1.1f;
        float tw = TW(lbl, ts);
        float lx = x0 + (kw - tw) * 0.5f;
        float ly = y0 + (KH - 7.f * ts) * 0.5f;
        DrawText(lx, ly, lbl, ts, tc);
    }
}

// Draw the preview strip — typed text with blinking cursor
static void DrawPreview()
{
    float x0 = PREV_X0;
    float y0 = PREV_Y;
    float x1 = PREV_X1;
    float y1 = y0 + PREV_H;

    FillRect(x0, y0, x1, y1, D3DCOLOR_XRGB(8, 12, 28));
    HLine(y0, x0, x1, COL_BORDER);
    HLine(y1, x0, x1, COL_BORDER);
    VLine(x0, y0, y1, COL_BORDER);
    VLine(x1, y0, y1, COL_BORDER);

    float textX = x0 + 8.f;
    float textY = y0 + (PREV_H - 7.f * 1.4f) * 0.5f;

    // Draw typed string
    if (s_buf && s_len > 0)
        DrawText(textX, textY, s_buf, 1.4f, COL_WHITE);

    // Blinking cursor — on for 30 frames, off for 30 frames
    bool cursorOn = (s_blink % 60) < 30;
    if (cursorOn)
    {
        float curX = textX + TW(s_buf ? s_buf : "", 1.4f);
        // Clamp cursor inside preview box
        if (curX < x1 - 4.f)
            FillRect(curX, textY, curX + 2.f, textY + 7.f * 1.4f, COL_CYAN);
    }
}

// ============================================================================
// Keyboard_Tick  — input + render, call every frame while active
// ============================================================================

void Keyboard_Tick(const DiagLogo& logo)
{
    if (!s_active) return;

    // ── Input ────────────────────────────────────────────────────────────────

    WORD cur = GetButtons();

    // L3 — toggle caps
    if (KBEdge(cur, BTN_LTHUMB))
        s_caps = !s_caps;

    // D-pad navigation
    if (KBEdge(cur, BTN_DPAD_LEFT))
    {
        s_col--;
        if (s_col < 0) s_col = RowLen(s_row) - 1;
    }
    if (KBEdge(cur, BTN_DPAD_RIGHT))
    {
        s_col++;
        if (s_col >= RowLen(s_row)) s_col = 0;
    }
    if (KBEdge(cur, BTN_DPAD_UP))
    {
        s_row--;
        if (s_row < 0) s_row = k_totalRows - 1;
        int maxCol = RowLen(s_row) - 1;
        if (s_col > maxCol) s_col = maxCol;
    }
    if (KBEdge(cur, BTN_DPAD_DOWN))
    {
        s_row++;
        if (s_row >= k_totalRows) s_row = 0;
        int maxCol = RowLen(s_row) - 1;
        if (s_col > maxCol) s_col = maxCol;
    }

    // A — type selected key
    if (KBEdge(cur, BTN_A))
    {
        if (s_row == 5)
        {
            // Function row
            if (s_col == FK_CAPS)
            {
                s_caps = !s_caps;
            }
            else if (s_col == FK_SPACE)
            {
                if (s_len < s_maxLen)
                {
                    s_buf[s_len++] = ' ';
                    s_buf[s_len] = '\0';
                }
            }
            else if (s_col == FK_BKSP)
            {
                if (s_len > 0)
                    s_buf[--s_len] = '\0';
            }
            else if (s_col == FK_DONE)
            {
                s_active = false;
                if (s_onDone) s_onDone(s_buf);
                s_prev = cur;
                return;
            }
        }
        else
        {
            // Typable key
            char c = KeyChar(s_row, s_col);
            if (c && s_len < s_maxLen)
            {
                s_buf[s_len++] = c;
                s_buf[s_len] = '\0';
                // One-shot caps: after typing an uppercase letter, drop back to lower
                // Only applies to letter rows (1-3), not symbols or number row
                // Disabled for now — user explicitly toggled caps, keep it until toggled off
            }
        }
    }

    // Y — space shortcut
    if (KBEdge(cur, BTN_Y))
    {
        if (s_len < s_maxLen)
        {
            s_buf[s_len++] = ' ';
            s_buf[s_len] = '\0';
        }
    }

    // X — backspace
    if (KBEdge(cur, BTN_X))
    {
        if (s_len > 0)
            s_buf[--s_len] = '\0';
    }

    // B — cancel
    if (KBEdge(cur, BTN_B))
    {
        s_active = false;
        if (s_onCancel) s_onCancel();
        s_prev = cur;
        return;
    }

    // START — confirm
    if (KBEdge(cur, BTN_START))
    {
        s_active = false;
        if (s_onDone) s_onDone(s_buf);
        s_prev = cur;
        return;
    }

    s_prev = cur;
    s_blink++;

    // ── Render ───────────────────────────────────────────────────────────────

    g_pDevice->BeginScene();

    // Build bottom bar hint string — show key shortcuts
    DrawPageChrome(logo, s_title ? s_title : "KEYBOARD",
        "[A] Type  [X] Bksp  [Y] Space  [L3] Caps  [Start] Done  [B] Cancel");

    // Preview strip
    DrawPreview();

    // Caps indicator — small label above keyboard grid
    {
        const char* capLbl = s_caps ? "CAPS ON" : "caps off";
        DWORD capCol = s_caps ? COL_GREEN : COL_DIM;
        float capX = k_gridX + k_gridW - TW(capLbl, 1.05f);
        float capY = KB_TOP - 16.f;
        DrawText(capX, capY, capLbl, 1.05f, capCol);
    }

    // Draw all keys
    for (int row = 0; row < k_totalRows; ++row)
    {
        int len = RowLen(row);
        for (int col = 0; col < len; ++col)
        {
            bool sel = (row == s_row && col == s_col);
            DrawKey(row, col, sel);
        }
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}