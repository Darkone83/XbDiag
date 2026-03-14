// ControllerTest.cpp
// XbDiag - Controller input visualizer
//
// Layout mirrors the physical Xbox controller shape:
//
//   LT [===]                              [===] RT
//
//   [BACK] [START]              [BLACK] [WHITE]
//
//        [L-STICK]                  [R-STICK]
//
//   [D-PAD]                                  [Y]
//                                          [X] [B]
//                                             [A]
//
// Exit: START+B held together
// Rumble subcard: START+A to enter, B to exit
//   LT -> left motor  (0-255)
//   RT -> right motor (0-255)
// All coordinates in 640x480 design units via SX/SY scaling.

#include "ControllerTest.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

extern void RequestState(int);
static const int ST_MENU = 0;

// ============================================================================
// Layout — all in 640x480 design units
// ============================================================================

// Horizontal anchors
// LT at 140 clears the logo which occupies ~0-230 in the top bar
// but content area is below top bar so X=80 is fine for content rows.
// However TrigBar label draws ABOVE the bar into the top bar region
// when Y_TRIG is small — fix by using absolute Y for trig labels.
static const float X_LEFT = 100.f;   // left group center-X
static const float X_RIGHT = 540.f;   // right group center-X
static const float X_MID = 320.f;   // screen center

// Vertical rows from CONTENT_Y
// CONTENT_Y=58, BOT_BAR_Y=450, usable height=392px
// TrigBar: label(16) + bar(52) + value(10) = 78px needs ~40px offset minimum
static const float Y_TRIG = 50.f;    // trigger bar center (label clears top chrome)
static const float Y_META = 122.f;   // BACK/START  BLACK/WHITE row
static const float Y_STICK = 185.f;   // analog stick center
static const float Y_DPAD = 335.f;   // d-pad cluster center
static const float Y_FACE = 335.f;   // face button cluster center

// Button box
static const float BW = 30.f;
static const float BH = 30.f;
static const float BG = 7.f;   // gap between boxes

// Wide button (BACK/START/BLACK/WHITE)
static const float WBW = 48.f;
static const float WBH = 20.f;

// Stick box half-size and dot
static const float SR = 40.f;
static const float DR = 5.f;

// Trigger bar
static const float TBW = 22.f;
static const float TBH = 52.f;

// ============================================================================
// State
// ============================================================================

static WORD s_prev = 0;
static bool s_rumbleMode = false;
static bool s_rumbleSkip = false;  // skip first tick on rumble entry — flush held buttons

// ============================================================================
// Helpers
// ============================================================================

static bool Edge(WORD cur, WORD prv, WORD btn)
{
    return (cur & btn) && !(prv & btn);
}

static void Box(float cx, float cy, float w, float h,
    const char* lbl, float lblScale, bool on)
{
    float x0 = cx - w * 0.5f;
    float y0 = cy - h * 0.5f;
    float x1 = cx + w * 0.5f;
    float y1 = cy + h * 0.5f;

    DWORD fill = on ? D3DCOLOR_XRGB(28, 170, 55) : D3DCOLOR_XRGB(12, 18, 40);
    DWORD border = on ? D3DCOLOR_XRGB(80, 255, 110) : COL_BORDER;
    DWORD tc = on ? COL_WHITE : COL_DIM;

    FillRect(x0, y0, x1, y1, fill);
    HLine(y0, x0, x1, border);
    HLine(y1, x0, x1, border);
    VLine(x0, y0, y1, border);
    VLine(x1, y0, y1, border);

    if (lbl && lbl[0])
        DrawText(cx - TW(lbl, lblScale) * 0.5f, cy - 5.f, lbl, lblScale, tc);
}

static void StickBox(float cx, float cy, int vx, int vy, bool click)
{
    float x0 = cx - SR;
    float y0 = cy - SR;
    float x1 = cx + SR;
    float y1 = cy + SR;

    DWORD border = click ? D3DCOLOR_XRGB(80, 255, 110) : COL_BORDER;

    FillRect(x0, y0, x1, y1, D3DCOLOR_XRGB(10, 14, 32));
    HLine(y0, x0, x1, border);
    HLine(y1, x0, x1, border);
    VLine(x0, y0, y1, border);
    VLine(x1, y0, y1, border);

    // Dim crosshair
    HLine(cy, x0 + 1.f, x1 - 1.f, D3DCOLOR_XRGB(25, 35, 65));
    VLine(cx, y0 + 1.f, y1 - 1.f, D3DCOLOR_XRGB(25, 35, 65));

    // Dot
    float range = SR - DR - 2.f;
    float dx = ((float)vx / 32767.f) * range;
    float dy = -((float)vy / 32767.f) * range;

    float dotX = cx + dx;
    float dotY = cy + dy;
    if (dotX < x0 + DR) dotX = x0 + DR;
    if (dotX > x1 - DR) dotX = x1 - DR;
    if (dotY < y0 + DR) dotY = y0 + DR;
    if (dotY > y1 - DR) dotY = y1 - DR;

    FillRect(dotX - DR, dotY - DR, dotX + DR, dotY + DR,
        D3DCOLOR_XRGB(0, 200, 255));
}

// Vertical trigger bar. cx/cy = center. val 0..255.
// Fills upward from bottom.
static void TrigBar(float cx, float cy, const char* lbl, int val)
{
    float x0 = cx - TBW * 0.5f;
    float y0 = cy - TBH * 0.5f;
    float x1 = cx + TBW * 0.5f;
    float y1 = cy + TBH * 0.5f;

    FillRect(x0, y0, x1, y1, D3DCOLOR_XRGB(10, 14, 32));

    float fillH = TBH * ((float)val / 255.f);
    if (fillH > 0.5f)
    {
        DWORD fc = val < 85 ? D3DCOLOR_XRGB(28, 170, 55) :
            val < 180 ? D3DCOLOR_XRGB(200, 175, 20) :
            D3DCOLOR_XRGB(210, 55, 30);
        FillRect(x0, y1 - fillH, x1, y1, fc);
    }

    // 25/50/75% ticks
    HLine(y0 + TBH * 0.25f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));
    HLine(y0 + TBH * 0.50f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));
    HLine(y0 + TBH * 0.75f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));

    HLine(y0, x0, x1, COL_BORDER);
    HLine(y1, x0, x1, COL_BORDER);
    VLine(x0, y0, y1, COL_BORDER);
    VLine(x1, y0, y1, COL_BORDER);

    // Label above bar
    DrawText(cx - TW(lbl, 1.2f) * 0.5f, y0 - 16.f, lbl, 1.2f, COL_GRAY);

    // Raw value below bar
    char t[6]; IntToStr(val, t, sizeof(t));
    DrawText(cx - TW(t, 1.05f) * 0.5f, y1 + 3.f, t, 1.05f, COL_DIM);
}

// ============================================================================
// OnEnter
// ============================================================================

void ControllerTest_OnEnter()
{
    s_prev = 0;
    s_rumbleMode = false;
    s_rumbleSkip = false;
}

// ============================================================================
// Rumble subcard render
// LT bar -> left motor, RT bar -> right motor, B exits
// ============================================================================

static void RenderRumble(const DiagLogo& logo, int lt, int rt)
{
    g_pDevice->BeginScene();

    DrawPageChrome(logo, "RUMBLE TEST",
        "[LT] Left motor   [RT] Right motor   [B] Exit");

    float B = CONTENT_Y;
    float cY = B + 160.f;   // vertical center of the card

    // ── Section header ──
    const char* hdr = "RUMBLE MOTORS";
    DrawText(X_MID - TW(hdr, 1.4f) * 0.5f, B + 30.f, hdr, 1.4f, COL_YELLOW);

    // ── Two large trigger bars, wider and taller than normal ──
    const float RBW = 40.f;   // rumble bar width
    const float RBH = 130.f;  // rumble bar height

    float lbCX = X_MID - 90.f;
    float rbCX = X_MID + 90.f;
    float barY = cY;   // center Y of both bars

    // Draw bars manually (wider than TrigBar helper)
    // Left motor
    {
        float x0 = lbCX - RBW * 0.5f;
        float y0 = barY - RBH * 0.5f;
        float x1 = lbCX + RBW * 0.5f;
        float y1 = barY + RBH * 0.5f;

        FillRect(x0, y0, x1, y1, D3DCOLOR_XRGB(10, 14, 32));

        float fillH = RBH * ((float)lt / 255.f);
        if (fillH > 0.5f)
        {
            DWORD fc = lt < 85 ? D3DCOLOR_XRGB(28, 170, 55) :
                lt < 180 ? D3DCOLOR_XRGB(200, 175, 20) :
                D3DCOLOR_XRGB(210, 55, 30);
            FillRect(x0, y1 - fillH, x1, y1, fc);
        }

        HLine(y0 + RBH * 0.25f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));
        HLine(y0 + RBH * 0.50f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));
        HLine(y0 + RBH * 0.75f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));
        HLine(y0, x0, x1, COL_BORDER);
        HLine(y1, x0, x1, COL_BORDER);
        VLine(x0, y0, y1, COL_BORDER);
        VLine(x1, y0, y1, COL_BORDER);

        const char* lbl = "LEFT";
        DrawText(lbCX - TW(lbl, 1.3f) * 0.5f, y0 - 20.f, lbl, 1.3f, COL_GRAY);

        char val[6]; IntToStr(lt, val, sizeof(val));
        DrawText(lbCX - TW(val, 1.1f) * 0.5f, y1 + 5.f, val, 1.1f, COL_WHITE);
    }

    // Right motor
    {
        float x0 = rbCX - RBW * 0.5f;
        float y0 = barY - RBH * 0.5f;
        float x1 = rbCX + RBW * 0.5f;
        float y1 = barY + RBH * 0.5f;

        FillRect(x0, y0, x1, y1, D3DCOLOR_XRGB(10, 14, 32));

        float fillH = RBH * ((float)rt / 255.f);
        if (fillH > 0.5f)
        {
            DWORD fc = rt < 85 ? D3DCOLOR_XRGB(28, 170, 55) :
                rt < 180 ? D3DCOLOR_XRGB(200, 175, 20) :
                D3DCOLOR_XRGB(210, 55, 30);
            FillRect(x0, y1 - fillH, x1, y1, fc);
        }

        HLine(y0 + RBH * 0.25f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));
        HLine(y0 + RBH * 0.50f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));
        HLine(y0 + RBH * 0.75f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));
        HLine(y0, x0, x1, COL_BORDER);
        HLine(y1, x0, x1, COL_BORDER);
        VLine(x0, y0, y1, COL_BORDER);
        VLine(x1, y0, y1, COL_BORDER);

        const char* lbl = "RIGHT";
        DrawText(rbCX - TW(lbl, 1.3f) * 0.5f, y0 - 20.f, lbl, 1.3f, COL_GRAY);

        char val[6]; IntToStr(rt, val, sizeof(val));
        DrawText(rbCX - TW(val, 1.1f) * 0.5f, y1 + 5.f, val, 1.1f, COL_WHITE);
    }

    // ── Combined % readout in the center ──
    {
        int pctL = (lt * 100) / 255;
        int pctR = (rt * 100) / 255;
        char comb[24];
        char tL[8], tR[8];
        IntToStr(pctL, tL, sizeof(tL));
        IntToStr(pctR, tR, sizeof(tR));
        StrCat3(comb, sizeof(comb), tL, "%  /  ", tR);
        char pct[28]; StrCat2(pct, sizeof(pct), comb, "%");
        DrawText(X_MID - TW(pct, 1.2f) * 0.5f, barY + RBH * 0.5f + 24.f,
            pct, 1.2f, COL_CYAN);
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// Render (main controller view)
// ============================================================================

static void Render(const DiagLogo& logo, WORD cur,
    int lx, int ly, int rx, int ry,
    int lt, int rt, int blk, int wht,
    int btnA, int btnB, int btnX, int btnY)
{
    float B = CONTENT_Y;  // base Y

    g_pDevice->BeginScene();

    DrawPageChrome(logo, "CONTROLLER TEST",
        "[START+A] Rumble   [START+B] Exit");

    // =========================================================
    // TRIGGERS  — top row, positioned like shoulder buttons
    // LT left, RT right
    // =========================================================

    TrigBar(X_LEFT, B + Y_TRIG, "LT", lt);
    TrigBar(X_RIGHT, B + Y_TRIG, "RT", rt);

    // =========================================================
    // META BUTTONS  — BACK/START (center-left), BLACK/WHITE (center-right)
    // =========================================================

    float metaY = B + Y_META;

    // BACK and START centered around X_MID - 80
    float metaLCX = X_MID - 90.f;
    Box(metaLCX - WBW * 0.5f - 4.f, metaY,
        WBW, WBH, "BACK", 1.1f, (cur & BTN_BACK) != 0);
    Box(metaLCX + WBW * 0.5f + 4.f, metaY,
        WBW, WBH, "START", 1.1f, (cur & BTN_START) != 0);

    // BLACK and WHITE centered around X_MID + 80
    float metaRCX = X_MID + 90.f;
    float blkCX = metaRCX - WBW * 0.5f - 4.f;
    float whtCX = metaRCX + WBW * 0.5f + 4.f;
    Box(blkCX, metaY, WBW, WBH, "BLK", 1.1f, (cur & BTN_BLACK) != 0);
    Box(whtCX, metaY, WBW, WBH, "WHT", 1.1f, (cur & BTN_WHITE) != 0);

    // Analog value readout below BLACK/WHITE
    {
        char bv[5], wv[5];
        IntToStr(blk, bv, sizeof(bv));
        IntToStr(wht, wv, sizeof(wv));
        DWORD bc = blk > 30 ? COL_CYAN : COL_DIM;
        DWORD wc = wht > 30 ? COL_CYAN : COL_DIM;
        DrawText(blkCX - TW(bv, 1.05f) * 0.5f, metaY + WBH * 0.5f + 4.f, bv, 1.05f, bc);
        DrawText(whtCX - TW(wv, 1.05f) * 0.5f, metaY + WBH * 0.5f + 4.f, wv, 1.05f, wc);
    }

    // =========================================================
    // ANALOG STICKS  — one third from each side
    // =========================================================

    float stickY = B + Y_STICK;

    StickBox(X_LEFT, stickY, lx, ly, (cur & BTN_LTHUMB) != 0);
    StickBox(X_RIGHT, stickY, rx, ry, (cur & BTN_RTHUMB) != 0);

    // Section labels above sticks
    DrawText(X_LEFT - TW("LEFT STICK", 1.2f) * 0.5f, stickY - SR - 18.f,
        "LEFT STICK", 1.2f, COL_YELLOW);
    DrawText(X_RIGHT - TW("RIGHT STICK", 1.2f) * 0.5f, stickY - SR - 18.f,
        "RIGHT STICK", 1.2f, COL_YELLOW);

    // L3/R3 click indicator below each stick
    DWORD l3c = (cur & BTN_LTHUMB) ? COL_GREEN : COL_DIM;
    DWORD r3c = (cur & BTN_RTHUMB) ? COL_GREEN : COL_DIM;
    DrawText(X_LEFT - TW("L3", 1.1f) * 0.5f, stickY + SR + 4.f, "L3", 1.1f, l3c);
    DrawText(X_RIGHT - TW("R3", 1.1f) * 0.5f, stickY + SR + 4.f, "R3", 1.1f, r3c);

    // =========================================================
    // D-PAD  — lower left
    // =========================================================

    float dpY = B + Y_DPAD;
    float dpCX = X_LEFT;

    // Label above U button top (U center = dpY-BH-BG, U top = that - BH/2)
    DrawText(dpCX - TW("D-PAD", 1.2f) * 0.5f, dpY - BH - BG - BH * 0.5f - 16.f,
        "D-PAD", 1.2f, COL_YELLOW);

    Box(dpCX, dpY - BH - BG, BW, BH, "U", 1.2f, (cur & BTN_DPAD_UP) != 0);
    Box(dpCX, dpY + BH + BG, BW, BH, "D", 1.2f, (cur & BTN_DPAD_DOWN) != 0);
    Box(dpCX - BW - BG, dpY, BW, BH, "L", 1.2f, (cur & BTN_DPAD_LEFT) != 0);
    Box(dpCX + BW + BG, dpY, BW, BH, "R", 1.2f, (cur & BTN_DPAD_RIGHT) != 0);
    // Center nub
    FillRect(dpCX - 5.f, dpY - 5.f, dpCX + 5.f, dpY + 5.f,
        D3DCOLOR_XRGB(20, 30, 55));

    // =========================================================
    // FACE BUTTONS  — lower right, Xbox diamond
    //   Y (top), X (left), B (right), A (bottom)
    // =========================================================

    float fY = B + Y_FACE;
    float fCX = X_RIGHT;

    DrawText(fCX - TW("BUTTONS", 1.2f) * 0.5f, fY - BH - BG - BH * 0.5f - 16.f,
        "BUTTONS", 1.2f, COL_YELLOW);

    Box(fCX, fY - BH - BG, BW, BH, "Y", 1.2f, (cur & BTN_Y) != 0);
    Box(fCX, fY + BH + BG, BW, BH, "A", 1.2f, (cur & BTN_A) != 0);
    Box(fCX - BW - BG, fY, BW, BH, "X", 1.2f, (cur & BTN_X) != 0);
    Box(fCX + BW + BG, fY, BW, BH, "B", 1.2f, (cur & BTN_B) != 0);

    // Analog value readout next to each face button
    {
        char va[5], vb[5], vx[5], vy[5];
        IntToStr(btnA, va, sizeof(va));
        IntToStr(btnB, vb, sizeof(vb));
        IntToStr(btnX, vx, sizeof(vx));
        IntToStr(btnY, vy, sizeof(vy));
        const float VS = 1.0f;
        DWORD ca = btnA > 30 ? COL_GREEN : COL_DIM;
        DWORD cb = btnB > 30 ? COL_RED : COL_DIM;
        DWORD cx = btnX > 30 ? COL_CYAN : COL_DIM;
        DWORD cy = btnY > 30 ? COL_YELLOW : COL_DIM;
        DrawText(fCX + BW + BG + 4.f, fY + BH + BG - 5.f, va, VS, ca);
        DrawText(fCX - BW - BG - TW(vy, VS), fY - BH - BG - 5.f, vy, VS, cy);
        DrawText(fCX - BW - BG - BW * 0.5f - TW(vx, VS), fY - 5.f, vx, VS, cx);
        DrawText(fCX + BW + BG + BW * 0.5f + 4.f, fY - 5.f, vb, VS, cb);
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// Tick
// ============================================================================

void ControllerTest_Tick(const DiagLogo& logo)
{
    WORD cur = GetButtons();

    int lx, ly, rx, ry;
    GetSticks(lx, ly, rx, ry);

    int lt, rt, blk, wht, btnA, btnB, btnX, btnY;
    GetTriggers(lt, rt, blk, wht, btnA, btnB, btnX, btnY);

    // ── Rumble subcard ──────────────────────────────────────────────────────
    if (s_rumbleMode)
    {
        // Skip first tick — A (entry button) may still be held, and trigger
        // values from the same frame as entry should not fire SetRumble yet
        if (s_rumbleSkip)
        {
            s_rumbleSkip = false;
            s_prev = cur;
            RenderRumble(logo, 0, 0);
            return;
        }

        // LT -> left motor, RT -> right motor
        // Trigger is 0-255, motor speed is WORD 0-65535 — scale up
        SetRumble((WORD)(lt * 257), (WORD)(rt * 257));

        // B (edge) exits subcard
        if (Edge(cur, s_prev, BTN_B))
        {
            SetRumble(0, 0);
            s_rumbleMode = false;
            s_prev = cur;
            return;
        }

        s_prev = cur;
        RenderRumble(logo, lt, rt);
        return;
    }

    // ── Main controller view ────────────────────────────────────────────────

    // START+A (edge on A while START held) -> enter rumble subcard
    if ((cur & BTN_START) && Edge(cur, s_prev, BTN_A))
    {
        s_rumbleMode = true;
        s_rumbleSkip = true;
        s_prev = cur;
        return;
    }

    // START+B -> exit to menu
    if ((cur & BTN_START) && (cur & BTN_B))
    {
        SetRumble(0, 0);
        RequestState(ST_MENU);
        s_prev = 0;
        return;
    }

    s_prev = cur;
    Render(logo, cur, lx, ly, rx, ry, lt, rt, blk, wht, btnA, btnB, btnX, btnY);
}