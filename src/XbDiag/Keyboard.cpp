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

// Function row key indices (row 5) — defined in Keyboard.h
static const int k_fnRowLen = FK_COUNT;  // 5 keys

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

// Row indent offsets — proper QWERTY left-biased stagger
// Row 0: 10 keys — no indent (number row, full width)
// Row 1: 10 keys — no indent (QWERTY row)
// Row 2:  9 keys — 0.5 key indent (ASDF row)
// Row 3:  7 keys — 1.0 key indent (ZXCV row)
// Row 4:  8 keys — 0.5 key indent (symbols row)
static const float k_rowIndent[5] =
{
    0.f,                        // row 0
    0.f,                        // row 1
    (KW + KG) * 0.5f,           // row 2
    (KW + KG) * 1.0f,           // row 3 — was 1.5, corrected to real QWERTY stagger
    (KW + KG) * 0.5f,           // row 4
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
static KeyboardMode     s_mode = KB_FULLSCREEN;
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
        // Fullscreen function row: CAPS, SPACE, BKSP, DONE — 4 keys
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
        return fx;  // FK_DONE (skip FK_CR — not shown fullscreen)
    }
    return k_gridX + k_rowIndent[row] + col * (KW + KG);
}

// Returns the width of a key
static float KeyW(int row, int col)
{
    if (row != 5) return KW;
    if (col == FK_CAPS)  return  60.f;
    if (col == FK_SPACE) return 240.f;
    if (col == FK_BKSP)  return  60.f;
    if (col == FK_CR)    return   0.f;  // not shown fullscreen
    return 80.f;  // FK_DONE
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
    KeyboardCancelFn onCancel,
    KeyboardMode     mode)
{
    s_title = title;
    s_buf = buf;
    s_maxLen = maxLen;
    s_onDone = onDone;
    s_onCancel = onCancel;
    s_mode = mode;

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

    // Selected key: bright outer glow border for maximum visibility
    if (selected)
    {
        DWORD glow = D3DCOLOR_XRGB(0, 255, 255);
        HLine(y0 - 1.f, x0 - 1.f, x1 + 1.f, glow);
        HLine(y1 + 1.f, x0 - 1.f, x1 + 1.f, glow);
        VLine(x0 - 1.f, y0 - 1.f, y1 + 1.f, glow);
        VLine(x1 + 1.f, y0 - 1.f, y1 + 1.f, glow);
    }

    // Key label
    const char* lbl = 0;
    char charBuf[2] = { 0, 0 };

    if (row == 5)
    {
        if (col == FK_CAPS)  lbl = s_caps ? "CAPS" : "caps";
        if (col == FK_SPACE) lbl = "SPACE";
        if (col == FK_BKSP)  lbl = "BKSP";
        if (col == FK_CR)    lbl = "Enter";  // skipped in fullscreen draw loop
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
// RenderOverlay  — keyboard panel drawn into an already-open scene.
// Caller owns BeginScene / EndScene / Present.
// Panel is pinned to the bottom of the screen.  Keys are compacted slightly
// to fit — same grid layout, smaller cell size.
// ============================================================================

// Overlay geometry (640x480)
// Panel sits in the bottom ~220px so file content stays readable above it.
static const float OV_PANEL_Y = 258.f;
static const float OV_PANEL_X0 = 0.f;
static const float OV_PANEL_X1 = 640.f;
static const float OV_PANEL_H = 480.f - OV_PANEL_Y;

// Compact key cell
static const float OKW = 32.f;   // key width
static const float OKH = 20.f;   // key height
static const float OKG = 3.f;   // gap

// Vertical layout inside the panel
static const float OV_TITLE_H = 14.f;
static const float OV_PREV_H = 18.f;
static const float OV_KB_TOP = OV_PANEL_Y + OV_TITLE_H + OV_PREV_H + 4.f;
static const float OV_ROW_STEP = OKH + OKG;

// QWERTY layout — grid centered accounting for BKSP side key on row 1
// Row 1: 10 keys (347px) + gap + BKSP (44px) = 394px total, centered in 640px
static const float OV_GRID_X = 123.f;

// Right-side extra key widths
static const float OV_BKSP_W = 44.f;  // BKSP on row 1
static const float OV_ENTER_W = 44.f;  // Enter on row 2

// Row indents — proper QWERTY left-biased stagger from OV_GRID_X
static const float k_ovRowIndent[5] =
{
    0.f,                        // row 0 — numbers
    0.f,                        // row 1 — QWERTY
    (OKW + OKG) * 0.5f,         // row 2 — ASDF
    (OKW + OKG) * 1.0f,         // row 3 — ZXCV
    (OKW + OKG) * 0.25f,        // row 4 — symbols, slight indent
};

// X position of a typable key in the overlay grid
static float OvKeyX(int row, int col)
{
    if (row == 5)
    {
        // Fn strip: CAPS | SPACE | DONE — 3 keys, centred
        // FK_BKSP and FK_CR are NOT on row 5 in overlay — they're on rows 1/2
        static const float FW_CAPS = 44.f;
        static const float FW_SPACE = 200.f;
        static const float FW_DONE = 60.f;
        static const float FW_TOTAL = FW_CAPS + FW_SPACE + FW_DONE + OKG * 2.f;
        float fx = (640.f - FW_TOTAL) * 0.5f;
        if (col == FK_CAPS)  return fx;
        fx += FW_CAPS + OKG;
        if (col == FK_SPACE) return fx;
        fx += FW_SPACE + OKG;
        return fx;  // FK_DONE
    }
    return OV_GRID_X + k_ovRowIndent[row] + col * (OKW + OKG);
}

// Width of a key in the overlay
static float OvKeyW(int row, int col)
{
    if (row != 5) return OKW;
    if (col == FK_CAPS)  return  44.f;
    if (col == FK_SPACE) return 200.f;
    return 60.f;  // FK_DONE
}

static float OvRowY(int row)
{
    return OV_KB_TOP + row * OV_ROW_STEP;
}

// Special column sentinels for side keys (BKSP on row 1, Enter on row 2)
// These are beyond the normal typable column range for their rows
static const int OV_COL_BKSP = 10;  // right of row 1 (QWERTY row has 10 keys, 0-9)
static const int OV_COL_ENTER = 9;   // right of row 2 (ASDF row has 9 keys, 0-8)

// X position of BKSP (right of row 1) and Enter (right of row 2)
static float OvSideKeyX(int row)
{
    float lastKeyRight = OV_GRID_X + k_ovRowIndent[row]
        + (float)k_rowLen[row] * (OKW + OKG) - OKG;
    return lastKeyRight + OKG;
}

static void DrawOvKey(float x0, float y0, float kw,
    const char* lbl, bool selected, bool capsActive)
{
    float x1 = x0 + kw;
    float y1 = y0 + OKH;

    DWORD fill, border, tc;
    if (selected)
    {
        fill = D3DCOLOR_XRGB(20, 70, 180);
        border = D3DCOLOR_XRGB(0, 220, 255);
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

    if (selected)
    {
        DWORD glow = D3DCOLOR_XRGB(0, 255, 255);
        HLine(y0 - 1.f, x0 - 1.f, x1 + 1.f, glow);
        HLine(y1 + 1.f, x0 - 1.f, x1 + 1.f, glow);
        VLine(x0 - 1.f, y0 - 1.f, y1 + 1.f, glow);
        VLine(x1 + 1.f, y0 - 1.f, y1 + 1.f, glow);
    }

    if (lbl && lbl[0])
    {
        float ts = 1.0f;
        float tw = TW(lbl, ts);
        float lx = x0 + (kw - tw) * 0.5f;
        float ly = y0 + (OKH - 7.f * ts) * 0.5f;
        DrawText(lx, ly, lbl, ts, tc);
    }
}

static void DrawKeyOverlay(int row, int col, bool selected)
{
    // Side keys (BKSP on row 1, Enter on row 2) are drawn by RenderOverlay directly
    if (row == 1 && col == OV_COL_BKSP)  return;
    if (row == 2 && col == OV_COL_ENTER) return;

    float x0 = OvKeyX(row, col);
    float y0 = OvRowY(row);
    float kw = OvKeyW(row, col);

    // Skip FK_BKSP and FK_CR on row 5 — they live on rows 1/2 now
    if (row == 5 && (col == FK_BKSP || col == FK_CR)) return;

    bool capsActive = (row == 5 && col == FK_CAPS && s_caps);

    const char* lbl = 0;
    char charBuf[2] = { 0, 0 };
    if (row == 5)
    {
        if (col == FK_CAPS)  lbl = s_caps ? "CAPS" : "caps";
        if (col == FK_SPACE) lbl = "SPACE";
        if (col == FK_DONE)  lbl = "DONE";
    }
    else
    {
        charBuf[0] = KeyChar(row, col);
        lbl = charBuf;
    }

    DrawOvKey(x0, y0, kw, lbl, selected, capsActive);
}

static void RenderOverlay()
{
    // ── Panel backdrop ────────────────────────────────────────────────────────
    FillRect(OV_PANEL_X0, OV_PANEL_Y, OV_PANEL_X1, 480.f,
        D3DCOLOR_XRGB(8, 11, 26));
    HLine(OV_PANEL_Y, OV_PANEL_X0, OV_PANEL_X1, COL_CYAN);

    // ── Title bar ─────────────────────────────────────────────────────────────
    {
        const char* t = s_title ? s_title : "KEYBOARD";
        DrawText(OV_PANEL_X0 + 6.f, OV_PANEL_Y + 3.f, t, 1.1f, COL_YELLOW);
        const char* capLbl = s_caps ? "CAPS ON" : "caps off";
        DWORD capCol = s_caps ? COL_GREEN : COL_DIM;
        DrawText(OV_PANEL_X1 - TW(capLbl, 1.0f) - 6.f,
            OV_PANEL_Y + 3.f, capLbl, 1.0f, capCol);
        HLine(OV_PANEL_Y + OV_TITLE_H, OV_PANEL_X0, OV_PANEL_X1, COL_BORDER);
    }

    // ── Preview strip ─────────────────────────────────────────────────────────
    {
        float py0 = OV_PANEL_Y + OV_TITLE_H;
        float py1 = py0 + OV_PREV_H;
        float textX = OV_PANEL_X0 + 8.f;
        float textY = py0 + (OV_PREV_H - 7.f * 1.2f) * 0.5f;

        FillRect(OV_PANEL_X0, py0, OV_PANEL_X1, py1, D3DCOLOR_XRGB(10, 14, 32));
        HLine(py1, OV_PANEL_X0, OV_PANEL_X1, COL_BORDER);

        if (s_buf && s_len > 0)
            DrawText(textX, textY, s_buf, 1.2f, COL_WHITE);

        bool cursorOn = (s_blink % 60) < 30;
        if (cursorOn)
        {
            float curX = textX + TW(s_buf ? s_buf : "", 1.2f);
            if (curX < OV_PANEL_X1 - 4.f)
                FillRect(curX, textY, curX + 2.f, textY + 7.f * 1.2f, COL_CYAN);
        }
    }

    // ── Key grid — typable rows ───────────────────────────────────────────────
    for (int row = 0; row < k_totalRows; ++row)
    {
        int len = RowLen(row);
        for (int col = 0; col < len; ++col)
        {
            // Skip FK_BKSP and FK_CR on row 5 — rendered as side keys
            if (row == 5 && (col == FK_BKSP || col == FK_CR)) continue;
            bool sel = (row == s_row && col == s_col);
            DrawKeyOverlay(row, col, sel);
        }
    }

    // ── BKSP — right side of row 1 (QWERTY row) ──────────────────────────────
    {
        float bx = OvSideKeyX(1);
        float by = OvRowY(1);
        bool  sel = (s_row == 1 && s_col == OV_COL_BKSP);
        DrawOvKey(bx, by, OV_BKSP_W, "BKSP", sel, false);
    }

    // ── Enter — right side of row 2 (ASDF row) ───────────────────────────────
    {
        float ex = OvSideKeyX(2);
        float ey = OvRowY(2);
        bool  sel = (s_row == 2 && s_col == OV_COL_ENTER);
        DrawOvKey(ex, ey, OV_ENTER_W, "Enter", sel, false);
    }

    // ── Hint line ─────────────────────────────────────────────────────────────
    {
        const char* hint = "[A] Type  [X] Bksp  [Y] Space  [L3] Caps  [Start] Done  [B] Cancel";
        DrawText((640.f - TW(hint, 0.95f)) * 0.5f, 480.f - 11.f, hint, 0.95f, COL_DIM);
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
            else if (s_col == FK_CR || s_col == FK_DONE)
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

    if (s_mode == KB_OVERLAY)
    {
        RenderOverlay();
    }
    else
    {
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

        // Draw all keys — skip FK_CR on fullscreen (Enter only needed in overlay)
        for (int row = 0; row < k_totalRows; ++row)
        {
            int len = RowLen(row);
            for (int col = 0; col < len; ++col)
            {
                if (row == 5 && col == FK_CR) continue;
                bool sel = (row == s_row && col == s_col);
                DrawKey(row, col, sel);
            }
        }

        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
    }
}

// ============================================================================
// FileViewer edit mode API — render and navigation without full Tick
// ============================================================================

void Keyboard_RenderOverlay()
{
    RenderOverlay();
}

void Keyboard_StepCol(int dir)
{
    // Row 1 has BKSP as an extra column at OV_COL_BKSP (10)
    // Row 2 has Enter as an extra column at OV_COL_ENTER (9)
    int maxCol = RowLen(s_row) - 1;
    if (s_row == 1) maxCol = OV_COL_BKSP;   // extend range to include BKSP
    if (s_row == 2) maxCol = OV_COL_ENTER;  // extend range to include Enter

    s_col += dir;
    if (s_col < 0)        s_col = maxCol;
    if (s_col > maxCol)   s_col = 0;
}

void Keyboard_StepRow(int dir)
{
    s_row += dir;
    if (s_row < 0)            s_row = k_totalRows - 1;
    if (s_row >= k_totalRows) s_row = 0;

    // Clamp col to new row's max (including side key columns)
    int maxCol = RowLen(s_row) - 1;
    if (s_row == 1) maxCol = OV_COL_BKSP;
    if (s_row == 2) maxCol = OV_COL_ENTER;
    if (s_col > maxCol) s_col = maxCol;
}

char Keyboard_GetSelectedChar()
{
    // Side key positions return 0 (not typable chars)
    if (s_row == 1 && s_col == OV_COL_BKSP)  return 0;
    if (s_row == 2 && s_col == OV_COL_ENTER) return 0;
    if (s_row == 5) return 0;
    return KeyChar(s_row, s_col);
}

int Keyboard_GetSelectedFnKey()
{
    // Side keys mapped to their FK_ values
    if (s_row == 1 && s_col == OV_COL_BKSP)  return FK_BKSP;
    if (s_row == 2 && s_col == OV_COL_ENTER) return FK_CR;
    if (s_row != 5) return -1;
    // Row 5 — but skip FK_BKSP and FK_CR slots (they're on rows 1/2 now)
    if (s_col == FK_BKSP || s_col == FK_CR) return -1;
    return s_col;
}

void Keyboard_ActivateSelected()
{
    // Only meaningful on the function row
    if (s_row != 5) return;
    if (s_col == FK_CAPS) { s_caps = !s_caps; }
    // SPACE and BKSP are handled directly by FileViewer via InsertChar/Backspace
    // DONE is handled by FileViewer via CommitLine
}

void Keyboard_ToggleCaps()
{
    s_caps = !s_caps;
}