// ============================================================================
// ScreenCalib.cpp — Runtime screen calibration / overscan adjustment
// ============================================================================

#include "ScreenCalib.h"
#include "DiagCommon.h"
#include "input.h"
#include "lcd.h"
#include <xtl.h>

// ── Local helpers ─────────────────────────────────────────────────────────────
static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return (cur & btn) != 0 && (prev & btn) == 0;
}

static bool StrEq(const char* a, const char* b)
{
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == '\0' && *b == '\0';
}

// ── Runtime margin globals — defined here, extern in ScreenCalib.h ──────────

// ── Internal state ────────────────────────────────────────────────────────────
static bool s_needsRun = false;   // true if no screen.set found at boot
static bool s_readOnly = false;   // true if write attempt failed

static const char* k_settingsPath = "D:\\screen.set";

// ── Helpers ───────────────────────────────────────────────────────────────────

static float Clamp(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void ApplyDefaults()
{
    // Zero margins = chrome fills full available area.
    // Correct for xemu and modern displays with no overscan.
    // CRT users who need compensation run calibration and save screen.set.
    g_marginL = 0.f;
    g_marginR = 0.f;
    g_marginT = CONTENT_Y;
    g_marginB = BOT_BAR_Y;
}

// ── Load / Save ───────────────────────────────────────────────────────────────

static bool LoadSettings()
{
    HANDLE hf = CreateFileA(k_settingsPath, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;

    char buf[256];
    DWORD nr = 0;
    ReadFile(hf, buf, sizeof(buf) - 1, &nr, NULL);
    CloseHandle(hf);
    buf[nr] = '\0';

    // Parse key=value lines — same pattern as XbSet
    const char* p = buf;
    while (*p)
    {
        // Skip whitespace and blank lines
        while (*p == ' ' || *p == '\r' || *p == '\n') ++p;
        if (!*p) break;

        // Read key
        char key[24]; int ki = 0;
        while (*p && *p != '=' && *p != '\r' && *p != '\n' && ki < 23)
            key[ki++] = *p++;
        key[ki] = '\0';
        if (*p == '=') ++p;

        // Read value as float via integer parse (* 100 then /100 for decimals)
        bool neg = false;
        if (*p == '-') { neg = true; ++p; }
        int whole = 0;
        while (*p >= '0' && *p <= '9') { whole = whole * 10 + (*p - '0'); ++p; }
        float val = (float)whole;
        if (neg) val = -val;
        while (*p && *p != '\r' && *p != '\n') ++p;

        if (StrEq(key, "MARGIN_L")) g_marginL = Clamp(val, CALIB_MARGIN_MIN, CALIB_MARGIN_MAX);
        else if (StrEq(key, "MARGIN_R")) g_marginR = Clamp(val, CALIB_MARGIN_MIN, CALIB_MARGIN_MAX);
        else if (StrEq(key, "MARGIN_T")) g_marginT = Clamp(val, CONTENT_Y, CONTENT_Y + 36.f);
        else if (StrEq(key, "MARGIN_B")) g_marginB = Clamp(val, BOT_BAR_Y - 36.f, BOT_BAR_Y);
    }
    return true;
}

static bool SaveSettings()
{
    // Ensure directory exists

    HANDLE hf = CreateFileA(k_settingsPath, GENERIC_WRITE, 0,
        NULL, CREATE_ALWAYS, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;

    char buf[128];
    char tmp[16];
    DWORD w;

    // Write each value as integer (margins are whole pixels)
    auto WL = [&](const char* key, float val)
        {
            char line[40];
            StrCopy(line, sizeof(line), key);
            StrCat2(line, sizeof(line), line, "=");
            IntToStr(Ftoi(val), tmp, sizeof(tmp));
            StrCat2(line, sizeof(line), line, tmp);
            StrCat2(line, sizeof(line), line, "\r\n");
            WriteFile(hf, line, StrLen(line), &w, NULL);
        };

    WL("MARGIN_L", g_marginL);
    WL("MARGIN_R", g_marginR);
    WL("MARGIN_T", g_marginT);
    WL("MARGIN_B", g_marginB);

    CloseHandle(hf);
    (void)buf;
    return true;
}

// ── Corner bracket drawing ────────────────────────────────────────────────────
// Each bracket is a 90-degree L-shape, ARM_LEN px per arm, at each screen corner.
// Drawn at the current calibration margin positions.

static const float ARM_LEN = 24.f;
static const float ARM_W = 2.f;

static void DrawBrackets(float ml, float mr, float mt, float mb, DWORD col)
{
    float right = SW - mr;
    float bottom = mb;

    // Top-left
    FillRect(ml, mt, ml + ARM_LEN, mt + ARM_W, col);  // H
    FillRect(ml, mt, ml + ARM_W, mt + ARM_LEN, col); // V

    // Top-right
    FillRect(right - ARM_LEN, mt, right, mt + ARM_W, col);  // H
    FillRect(right - ARM_W, mt, right, mt + ARM_LEN, col); // V

    // Bottom-left
    FillRect(ml, bottom - ARM_W, ml + ARM_LEN, bottom, col);  // H
    FillRect(ml, bottom - ARM_LEN, ml + ARM_W, bottom, col); // V

    // Bottom-right
    FillRect(right - ARM_LEN, bottom - ARM_W, right, bottom, col);  // H
    FillRect(right - ARM_W, bottom - ARM_LEN, right, bottom, col); // V
}

// ── Calibration render ────────────────────────────────────────────────────────

static void RenderCalib(float ml, float mr, float mt, float mb, bool readOnly)
{
    g_pDevice->BeginScene();

    // Fill entire screen black -- must cover every pixel so user sees true edges
    FillRect(0.f, 0.f, SW, SH, D3DCOLOR_XRGB(0, 0, 0));

    // Chrome boundary positions -- mirrors DrawPageChrome exactly
    float topEdge = mt - TOP_BAR_H;   // top of top bar
    float botEdge = mb + BOT_BAR_H;   // bottom of bottom bar
    float lftEdge = ml;               // left content edge
    float rgtEdge = SW - mr;          // right content edge

    // Faint full-screen border at absolute screen edges as reference
    HLine(0.f, 0.f, SW, D3DCOLOR_XRGB(20, 25, 40));
    HLine(SH - 1.f, 0.f, SW, D3DCOLOR_XRGB(20, 25, 40));
    VLine(0.f, 0.f, SH, D3DCOLOR_XRGB(20, 25, 40));
    VLine(SW - 1.f, 0.f, SH, D3DCOLOR_XRGB(20, 25, 40));

    // Pulsing brackets at chrome boundary positions
    DWORD t = GetTickCount() % 1600;
    float phase = (float)t / 1600.f;
    float pulse = (phase < 0.5f) ? (phase * 2.f) : (2.f - phase * 2.f);
    BYTE  br = (BYTE)(160 + Ftoi(pulse * 95.f));
    BYTE  bg = (BYTE)(200 + Ftoi(pulse * 55.f));
    BYTE  bb = (BYTE)(160 + Ftoi(pulse * 95.f));
    DWORD bracketCol = D3DCOLOR_XRGB(br, bg, bb);

    DrawBrackets(lftEdge, SW - rgtEdge, topEdge, botEdge, bracketCol);

    // Centre instructions absolutely in the bracket area
    // Count total text block height first, then position so it's truly centred
    // Lines: title(1.5) + gap + subtitle(1.2) + gap + 4 instruction lines(1.1) + gap + vals(1.1) = 7 rows
    const float TITLE_H = LINE_H * 1.5f;
    const float BODY_H = LINE_H * 1.2f;
    const float INST_H = LINE_H * 1.1f;
    const int   N_INST = 4;  // instruction lines
    float totalTextH = TITLE_H + LINE_H          // title + gap
        + BODY_H + LINE_H * 0.5f   // subtitle + gap
        + INST_H * N_INST            // 4 instruction lines
        + LINE_H                     // gap before vals
        + INST_H;                    // vals line
    if (readOnly) totalTextH += LINE_H + INST_H; // extra read-only notice

    float areaH = botEdge - topEdge;
    float areaW = rgtEdge - lftEdge;
    float blockT = topEdge + (areaH - totalTextH) * 0.5f;
    float cx = lftEdge + areaW * 0.5f;       // true horizontal centre of calibrated area

    float cy = blockT;
    DrawTextC(cx, cy, "SCREEN CALIBRATION", 1.5f, COL_CYAN);
    cy += TITLE_H + LINE_H * 0.5f;
    DrawTextC(cx, cy, "Adjust brackets to screen edges", 1.2f, COL_WHITE);
    cy += BODY_H + LINE_H * 0.5f;
    DrawTextC(cx, cy, "[DPAD Left/Right]  Left / Right margin", 1.1f, COL_GRAY); cy += INST_H;
    DrawTextC(cx, cy, "[DPAD Up/Down]     Top / Bottom margin", 1.1f, COL_GRAY); cy += INST_H;
    DrawTextC(cx, cy, "[LT] Fine (1px)  [RT] Coarse (4px)", 1.1f, COL_GRAY); cy += INST_H;
    DrawTextC(cx, cy, "[BLACK] Reverse   [A] Save   [B] Cancel", 1.1f, COL_GRAY); cy += INST_H;

    // Current values
    char vals[64];
    char n1[8], n2[8], n3[8], n4[8];
    IntToStr(Ftoi(ml), n1, sizeof(n1));
    IntToStr(Ftoi(mr), n2, sizeof(n2));
    IntToStr(Ftoi(mt), n3, sizeof(n3));
    IntToStr(Ftoi(mb), n4, sizeof(n4));
    StrCopy(vals, sizeof(vals), "L:");
    StrCat2(vals, sizeof(vals), vals, n1);
    StrCat2(vals, sizeof(vals), vals, "  R:");
    StrCat2(vals, sizeof(vals), vals, n2);
    StrCat2(vals, sizeof(vals), vals, "  T:");
    StrCat2(vals, sizeof(vals), vals, n3);
    StrCat2(vals, sizeof(vals), vals, "  B:");
    StrCat2(vals, sizeof(vals), vals, n4);
    DrawTextC(SW * 0.5f, cy, vals, 1.1f, COL_DIM);

    if (readOnly)
    {
        cy += LINE_H + 6.f;
        DrawTextC(SW * 0.5f, cy,
            "Note: Settings cannot be saved — running from read-only media",
            1.1f, COL_ORANGE);
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ── Public API ────────────────────────────────────────────────────────────────

void ScreenCalib_Init(const DiagLogo& logo)
{
    (void)logo;
    ApplyDefaults();
    if (LoadSettings())
        s_needsRun = false;
    else
        s_needsRun = true;
}

bool ScreenCalib_NeedsRun()
{
    return s_needsRun;
}

bool ScreenCalib_IsReadOnly()
{
    return s_readOnly;
}

void ScreenCalib_Run(const DiagLogo& logo)
{
    (void)logo;

    // Single horizontal margin (ml=mr) and single vertical margin (vMargin).
    // vMargin is the inset from the natural content boundaries:
    //   g_marginT = CONTENT_Y + vMargin  (top bar shifts down)
    //   g_marginB = BOT_BAR_Y - vMargin  (bottom bar shifts up)
    // This keeps the model symmetrical and intuitive:
    //   DPAD Right = push brackets inward (increase h margin)
    //   DPAD Left  = push brackets outward (decrease h margin)
    //   DPAD Down  = push brackets inward vertically (increase vMargin)
    //   DPAD Up    = push brackets outward vertically (decrease vMargin)

    float hMargin = g_marginL;                    // start from current setting
    float vMargin = g_marginT - CONTENT_Y;        // derive from current setting

    // Determine read-only state by attempting to create the file
    {
        HANDLE hTest = CreateFileA(k_settingsPath, GENERIC_WRITE, 0,
            NULL, CREATE_ALWAYS, 0, NULL);
        if (hTest == INVALID_HANDLE_VALUE)
            s_readOnly = true;
        else
        {
            s_readOnly = false;
            CloseHandle(hTest);
        }
    }

    WORD prev = GetButtons();
    bool reverse = false;

    while (true)
    {
        PumpInput();
        WORD cur = GetButtons();

        float step = (cur & BTN_RTRIG) ? CALIB_STEP_COARSE : CALIB_STEP_FINE;
        if (reverse) step = -step;

        if (EdgeDown(cur, prev, BTN_BLACK))
            reverse = !reverse;

        if (EdgeDown(cur, prev, BTN_DPAD_RIGHT))
            hMargin = Clamp(hMargin + step, CALIB_MARGIN_MIN, CALIB_MARGIN_MAX);
        if (EdgeDown(cur, prev, BTN_DPAD_LEFT))
            hMargin = Clamp(hMargin - step, CALIB_MARGIN_MIN, CALIB_MARGIN_MAX);
        if (EdgeDown(cur, prev, BTN_DPAD_DOWN))
            vMargin = Clamp(vMargin + step, 0.f, 36.f);
        if (EdgeDown(cur, prev, BTN_DPAD_UP))
            vMargin = Clamp(vMargin - step, 0.f, 36.f);

        // Derive actual margin globals from the simplified model
        float ml = hMargin;
        float mr = hMargin;
        float mt = CONTENT_Y + vMargin;
        float mb = BOT_BAR_Y - vMargin;

        // Confirm
        if (EdgeDown(cur, prev, BTN_A))
        {
            g_marginL = ml;
            g_marginR = mr;
            g_marginT = mt;
            g_marginB = mb;
            if (!s_readOnly)
                SaveSettings();
            s_needsRun = false;
            prev = cur;
            break;
        }

        // Cancel
        if (EdgeDown(cur, prev, BTN_B))
        {
            prev = cur;
            break;
        }

        prev = cur;
        RenderCalib(ml, mr, mt, mb, s_readOnly);
        LCD_Tick(cur, prev);
    }
}