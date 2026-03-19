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
// Stick Test card  (START+DPAD_UP to enter, B to exit)
// Three sub-tests cycled with LEFT/RIGHT dpad:
//   0 = Dead-zone   — raw XY + dead-zone radius ring visualised on stick plot
//   1 = Circularity — traces stick path, shows gate shape vs ideal circle
//   2 = Drift       — samples at-rest position, reports average XY offset
// ============================================================================
static bool s_stickMode = false;
static int  s_stickTest = 0;    // 0/1/2

// Circularity: 36 angular buckets (10° each), max normalised radius per bucket
static const int CIRC_BUCKETS = 36;
static float     s_circMaxR[CIRC_BUCKETS];   // left stick
static float     s_circMaxRR[CIRC_BUCKETS];  // right stick
static bool      s_circHasData = false;
static bool      s_circHasDataR = false;

// Drift: ring buffer of raw at-rest samples
static const int DRIFT_SAMPLES = 180;   // ~3s at 60fps
static int  s_driftLX[DRIFT_SAMPLES];
static int  s_driftLY[DRIFT_SAMPLES];
static int  s_driftRX[DRIFT_SAMPLES];
static int  s_driftRY[DRIFT_SAMPLES];
static int  s_driftHead = 0;
static int  s_driftCount = 0;
static int  s_driftWarmup = 0;   // frames to discard before sampling (settle delay)
static int  s_driftAvgLX = 0, s_driftAvgLY = 0;
static int  s_driftAvgRX = 0, s_driftAvgRY = 0;

// ADC jitter null-out: after warmup, capture a short baseline then clamp
// samples within ±ADC_JITTER of that baseline to the baseline value.
// Xbox ADC has 2-4% noise at rest (~655-1310 counts) — clamp covers that floor.
static const int ADC_JITTER = 655;  // 2% of 32767 — nulls expected ADC noise
static const int BASELINE_FRAMES = 10;  // frames to average for the baseline
static int  s_driftBaselineLX = 0, s_driftBaselineLY = 0;
static int  s_driftBaselineRX = 0, s_driftBaselineRY = 0;
static int  s_driftBaselineCount = 0;   // 0 = not yet captured
static int  s_driftBaselineAccLX = 0, s_driftBaselineAccLY = 0;
static int  s_driftBaselineAccRX = 0, s_driftBaselineAccRY = 0;


// Trigger dead-zone card state
static const int TRIG_DZ_THRESHOLD = 30;   // Xbox trigger dead-zone (0-255 range)
static int  s_trigLT = 0;    // current raw LT value
static int  s_trigRT = 0;    // current raw RT value
static int  s_trigMinLT = 255, s_trigMaxLT = 0;
static int  s_trigMinRT = 255, s_trigMaxRT = 0;
static bool s_trigHasData = false;

// Per-port disconnect tracking.
// s_wasConn:    connection state from the previous tick (for edge detection).
// s_discCount:  running count of disconnect events seen this session.
static bool s_wasConn[4] = { false, false, false, false };
static int  s_discCount[4] = { 0, 0, 0, 0 };

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
    s_stickMode = false;
    s_stickTest = 0;
    s_circHasData = false;
    s_circHasDataR = false;
    for (int i = 0; i < CIRC_BUCKETS; ++i) { s_circMaxR[i] = 0.f; s_circMaxRR[i] = 0.f; }
    s_driftHead = 0; s_driftCount = 0; s_driftWarmup = 60; s_driftBaselineCount = 0; s_driftBaselineAccLX = 0; s_driftBaselineAccLY = 0; s_driftBaselineAccRX = 0; s_driftBaselineAccRY = 0;
    s_driftAvgLX = 0; s_driftAvgLY = 0;
    s_driftAvgRX = 0; s_driftAvgRY = 0; s_driftBaselineLX = 0; s_driftBaselineLY = 0; s_driftBaselineRX = 0; s_driftBaselineRY = 0;
    for (int p = 0; p < 4; ++p)
    {
        s_wasConn[p] = IsPortConnected(p);
        s_discCount[p] = 0;
    }
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
    float cY = B + 185.f;   // vertical center of the card (shifted down to clear port strip)

    // ── Port status strip (mirrors main page) ──────────────────────────────
    {
        const float PBW = 46.f;
        const float PBH = 20.f;
        const float PGAP = 6.f;
        const float totalW = PBW * 4.f + PGAP * 3.f;
        float px = X_MID - totalW * 0.5f;
        float py = B + 8.f;

        const char* portLabels[4] = { "1", "2", "3", "4" };
        for (int p = 0; p < 4; ++p)
        {
            bool conn = IsPortConnected(p);
            float x0 = px + (PBW + PGAP) * (float)p;
            float x1 = x0 + PBW;
            float y0 = py;
            float y1 = py + PBH;
            float cx = (x0 + x1) * 0.5f;
            float cy = (y0 + y1) * 0.5f;

            DWORD fill = conn ? D3DCOLOR_XRGB(10, 40, 18) : D3DCOLOR_XRGB(10, 14, 32);
            DWORD border = conn ? D3DCOLOR_XRGB(80, 255, 110) : COL_BORDER;
            DWORD tc = conn ? COL_GREEN : COL_DIM;

            FillRect(x0, y0, x1, y1, fill);
            HLine(y0, x0, x1, border);
            HLine(y1, x0, x1, border);
            VLine(x0, y0, y1, border);
            VLine(x1, y0, y1, border);
            DrawText(cx - TW(portLabels[p], 1.2f) * 0.5f, cy - 5.f,
                portLabels[p], 1.2f, tc);
        }

        // Disconnects row
        float discY = py + PBH + 4.f;
        DrawText(px - 84.f, discY, "Disconnects:", 1.1f, COL_GRAY);
        for (int p = 0; p < 4; ++p)
        {
            float cx = px + (PBW + PGAP) * (float)p + PBW * 0.5f;
            char buf[8];
            IntToStr(s_discCount[p], buf, sizeof(buf));
            DWORD cc = s_discCount[p] > 0 ? COL_RED : COL_DIM;
            DrawText(cx - TW(buf, 1.2f) * 0.5f, discY, buf, 1.2f, cc);
        }
    }

    // ── Section header ──
    const char* hdr = "RUMBLE MOTORS";
    DrawText(X_MID - TW(hdr, 1.4f) * 0.5f, B + 58.f, hdr, 1.4f, COL_YELLOW);

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
// ============================================================================
// Stick test helpers
// ============================================================================

// Integer square root (no __ftol2_sse — x87 inline)
static int ISqrt(int v)
{
    if (v <= 0) return 0;
    float fv = (float)v;
    float r;
    __asm { fld fv }
    __asm { fsqrt }
    __asm { fstp r }
    int ri;
    __asm { fld r }
    __asm { fistp ri }
    return ri;
}

// Normalised radius 0.0-1.0 from raw 16-bit stick axes
static float StickRadius(int x, int y)
{
    // max reach of each axis is 32767
    float fx = (float)x / 32767.f;
    float fy = (float)y / 32767.f;
    float r2 = fx * fx + fy * fy;
    if (r2 <= 0.f) return 0.f;
    float r;
    __asm { fld r2 }
    __asm { fsqrt }
    __asm { fstp r }
    return r > 1.f ? 1.f : r;
}

// Angle bucket 0-35 from raw axes (0=East, CCW, 10° each).
// 36 buckets (10° per bucket) — wide enough that a full rotation cleanly
// fills every bucket even with natural stick wobble.
// Y is negated so bucket angle matches screen convention (stick up = top of plot).
static int AngleBucket(int x, int y)
{
    if (x == 0 && y == 0) return 0;
    float fx = (float)x;
    float fy = -(float)y;   // negate: Xbox Y+ is up, screen Y+ is down
    float ang;
    __asm { fld fy }
    __asm { fld fx }
    __asm { fpatan }        // atan2(ST1, ST0) = atan2(fy, fx)
    __asm { fstp ang }

    // Convert -pi..pi to 0..2pi
    if (ang < 0.f)
    {
        float twopi = 6.2831853f;
        ang = ang + twopi;
    }

    // Scale to 0..36. Subtract small epsilon so exact boundaries floor down.
    float bucketF = ang * (36.f / 6.2831853f) - 0.0001f;
    if (bucketF < 0.f) bucketF = 0.f;
    int bucket;
    __asm { fld bucketF }
    __asm { fistp bucket }
    if (bucket < 0)   bucket = 0;
    if (bucket >= 36) bucket = 0;   // wrap: 2*pi rounds to exactly 36
    return bucket;
}

// Draw a stick plot (square + dot) at cx,cy with radius r
// Optionally draws a dead-zone ring at dzRadius (0 = skip)
static void StickPlot(float cx, float cy, float r,
    int vx, int vy, float dzRadius, DWORD dotCol)
{
    float x0 = cx - r;
    float y0 = cy - r;
    float x1 = cx + r;
    float y1 = cy + r;

    FillRect(x0, y0, x1, y1, D3DCOLOR_XRGB(10, 14, 32));
    HLine(y0, x0, x1, COL_BORDER);
    HLine(y1, x0, x1, COL_BORDER);
    VLine(x0, y0, y1, COL_BORDER);
    VLine(x1, y0, y1, COL_BORDER);
    HLine(cy, x0 + 1.f, x1 - 1.f, D3DCOLOR_XRGB(25, 35, 65));
    VLine(cx, y0 + 1.f, y1 - 1.f, D3DCOLOR_XRGB(25, 35, 65));

    // Dead-zone ring — drawn as a simple square inset
    if (dzRadius > 0.f)
    {
        float dz = r * dzRadius;
        HLine(cy - dz, cx - dz, cx + dz, D3DCOLOR_XRGB(60, 60, 20));
        HLine(cy + dz, cx - dz, cx + dz, D3DCOLOR_XRGB(60, 60, 20));
        VLine(cx - dz, cy - dz, cy + dz, D3DCOLOR_XRGB(60, 60, 20));
        VLine(cx + dz, cy - dz, cy + dz, D3DCOLOR_XRGB(60, 60, 20));
    }

    // Dot
    float range = r - 5.f;
    float dx = ((float)vx / 32767.f) * range;
    float dy = -((float)vy / 32767.f) * range;
    float dotX = cx + dx;
    float dotY = cy + dy;
    if (dotX < x0 + 4.f) dotX = x0 + 4.f;
    if (dotX > x1 - 4.f) dotX = x1 - 4.f;
    if (dotY < y0 + 4.f) dotY = y0 + 4.f;
    if (dotY > y1 - 4.f) dotY = y1 - 4.f;
    FillRect(dotX - 4.f, dotY - 4.f, dotX + 4.f, dotY + 4.f, dotCol);
}

// ============================================================================
// Stick Test card render
// ============================================================================

// Draw one circularity plot at (cx,cy) radius PR from a bucket array.
// Also draws the live stick dot at (vx,vy).
static void CircPlot(float cx, float cy, float PR,
    float* maxR, int vx, int vy)
{
    // Background square
    FillRect(cx - PR, cy - PR, cx + PR, cy + PR, D3DCOLOR_XRGB(10, 14, 32));

    // Boundary ring — midpoint circle algorithm, 1x1 pixel dots.
    {
        float _prf = PR + 0.5f;
        int ir;
        __asm { fld _prf }
        __asm { fistp ir }
        int px2 = ir, py2 = 0, err = 1 - ir;
        while (px2 >= py2)
        {
            float pts[8][2] = {
                { cx + (float)px2, cy - (float)py2 },
                { cx + (float)py2, cy - (float)px2 },
                { cx - (float)py2, cy - (float)px2 },
                { cx - (float)px2, cy - (float)py2 },
                { cx - (float)px2, cy + (float)py2 },
                { cx - (float)py2, cy + (float)px2 },
                { cx + (float)py2, cy + (float)px2 },
                { cx + (float)px2, cy + (float)py2 },
            };
            for (int i = 0; i < 8; ++i)
                FillRect(pts[i][0], pts[i][1], pts[i][0] + 1.f, pts[i][1] + 1.f, COL_BORDER);
            py2++;
            if (err < 0) err += 2 * py2 + 1;
            else { px2--; err += 2 * (py2 - px2) + 1; }
        }
    }

    // Crosshairs
    HLine(cy, cx - PR + 1.f, cx + PR - 1.f, D3DCOLOR_XRGB(25, 35, 65));
    VLine(cx, cy - PR + 1.f, cy + PR - 1.f, D3DCOLOR_XRGB(25, 35, 65));

    // Bucket dots — primary bucket only, no smear.
    // Green = gate reached (>= 0.7), amber = partial contact.
    for (int b = 0; b < CIRC_BUCKETS; ++b)
    {
        if (maxR[b] < 0.05f) continue;
        float angF = ((float)b + 0.5f) * (6.2831853f / (float)CIRC_BUCKETS);
        float cosA, sinA;
        __asm { fld angF }
        __asm { fcos }
        __asm { fstp cosA }
        __asm { fld angF }
        __asm { fsin }
        __asm { fstp sinA }
        float reach = maxR[b];
        if (reach > 1.f) reach = 1.f;
        float px2 = cx + cosA * reach * (PR - 2.f);
        float py2 = cy - sinA * reach * (PR - 2.f);
        DWORD dc = reach >= 0.7f ? COL_GREEN : D3DCOLOR_XRGB(200, 140, 0);
        FillRect(px2 - 2.f, py2 - 2.f, px2 + 2.f, py2 + 2.f, dc);
    }

    // Live dot — normalised to unit circle, clamped
    float dxf = (float)vx / 32767.f;
    float dyf = -((float)vy / 32767.f);
    float dotDist = dxf * dxf + dyf * dyf;
    if (dotDist > 1.f)
    {
        float inv;
        __asm { fld dotDist }
        __asm { fsqrt }
        __asm { fstp inv }
        dxf /= inv;
        dyf /= inv;
    }
    float dotX = cx + dxf * (PR - 3.f);
    float dotY = cy + dyf * (PR - 3.f);
    FillRect(dotX - 3.f, dotY - 3.f, dotX + 3.f, dotY + 3.f, COL_CYAN);
}

static void RenderStickTest(const DiagLogo& logo, int lx, int ly, int rx, int ry,
    int rawLX, int rawLY, int rawRX, int rawRY)
{
    g_pDevice->BeginScene();

    const char* testNames[4] = { "DEAD-ZONE", "CIRCULARITY", "DRIFT", "TRIGGERS" };
    const char* hint = "[Left/Right] Switch test    [B] Exit";
    DrawPageChrome(logo, "STICK TEST", hint);

    float B = CONTENT_Y;

    // ── Tab strip ──────────────────────────────────────────────────────────
    {
        const float TW2 = 88.f;
        const float TH = 16.f;
        const float TG = 4.f;
        float tx = X_MID - (TW2 * 4.f + TG * 3.f) * 0.5f;
        float ty = B;
        for (int i = 0; i < 4; ++i)
        {
            bool sel = (i == s_stickTest);
            float x0 = tx + (TW2 + TG) * (float)i;
            float x1 = x0 + TW2;
            FillRectGrad(x0, ty, x1, ty + TH,
                sel ? D3DCOLOR_XRGB(30, 80, 160) : D3DCOLOR_XRGB(18, 25, 55),
                sel ? D3DCOLOR_XRGB(15, 50, 110) : D3DCOLOR_XRGB(12, 16, 38));
            HLine(ty, x0, x1, sel ? COL_CYAN : COL_BORDER);
            float lx2 = x0 + (x1 - x0) * 0.5f - TW(testNames[i], 1.1f) * 0.5f;
            DrawText(lx2, ty + 3.f, testNames[i], 1.1f, sel ? COL_WHITE : COL_DIM);
        }
    }

    float contentY = B + 22.f;

    // ── DEAD-ZONE test ─────────────────────────────────────────────────────
    if (s_stickTest == 0)
    {
        const float PR = 80.f;   // plot radius

        // Left stick
        float lcx = X_MID - 130.f;
        float lcy = contentY + 110.f;
        float lRadius = StickRadius(lx, ly);
        // Dead-zone: threshold at ~7000/32767 ≈ 0.21 (STICK_DEADZONE from input.cpp)
        float dzN = 8000.f / 32767.f;
        StickPlot(lcx, lcy, PR, lx, ly, dzN,
            lRadius > dzN ? COL_GREEN : D3DCOLOR_XRGB(100, 100, 100));

        DrawText(lcx - TW("LEFT STICK", 1.15f) * 0.5f, lcy - PR - 16.f,
            "LEFT STICK", 1.15f, COL_YELLOW);

        // Raw readout below
        char xbuf[10], ybuf[10], rbuf[6];
        IntToStr(lx, xbuf, sizeof(xbuf));
        IntToStr(ly, ybuf, sizeof(ybuf));
        // radius as integer 0-100
        int lrpct;
        { float f = lRadius * 100.f; __asm { fld f } __asm { fistp lrpct } }
        IntToStr(lrpct, rbuf, sizeof(rbuf));
        float ry2 = lcy + PR + 6.f;
        DrawText(lcx - 60.f, ry2, "X:", 1.1f, COL_GRAY);
        DrawText(lcx - 40.f, ry2, xbuf, 1.1f, COL_WHITE);
        DrawText(lcx - 60.f, ry2 + 14.f, "Y:", 1.1f, COL_GRAY);
        DrawText(lcx - 40.f, ry2 + 14.f, ybuf, 1.1f, COL_WHITE);
        DrawText(lcx + 10.f, ry2, "R%:", 1.1f, COL_GRAY);
        DrawText(lcx + 36.f, ry2, rbuf, 1.1f,
            lRadius > dzN ? COL_GREEN : COL_DIM);

        // Right stick
        float rcx = X_MID + 130.f;
        float rcy = contentY + 110.f;
        float rRadius = StickRadius(rx, ry);
        StickPlot(rcx, rcy, PR, rx, ry, dzN,
            rRadius > dzN ? COL_GREEN : D3DCOLOR_XRGB(100, 100, 100));

        DrawText(rcx - TW("RIGHT STICK", 1.15f) * 0.5f, rcy - PR - 16.f,
            "RIGHT STICK", 1.15f, COL_YELLOW);

        char rxbuf[10], rybuf[10], rrbuf[6];
        IntToStr(rx, rxbuf, sizeof(rxbuf));
        IntToStr(ry, rybuf, sizeof(rybuf));
        int rrpct;
        { float f = rRadius * 100.f; __asm { fld f } __asm { fistp rrpct } }
        IntToStr(rrpct, rrbuf, sizeof(rrbuf));
        DrawText(rcx - 60.f, ry2, "X:", 1.1f, COL_GRAY);
        DrawText(rcx - 40.f, ry2, rxbuf, 1.1f, COL_WHITE);
        DrawText(rcx - 60.f, ry2 + 14.f, "Y:", 1.1f, COL_GRAY);
        DrawText(rcx - 40.f, ry2 + 14.f, rybuf, 1.1f, COL_WHITE);
        DrawText(rcx + 10.f, ry2, "R%:", 1.1f, COL_GRAY);
        DrawText(rcx + 36.f, ry2, rrbuf, 1.1f,
            rRadius > dzN ? COL_GREEN : COL_DIM);

        // Centre: dead-zone legend
        DrawText(X_MID - TW("Dead-zone threshold", 1.1f) * 0.5f,
            contentY + 240.f, "Dead-zone threshold", 1.1f, COL_GRAY);
        DrawText(X_MID - TW("shown as yellow ring", 1.05f) * 0.5f,
            contentY + 254.f, "shown as yellow ring", 1.05f, D3DCOLOR_XRGB(60, 60, 20));
        DrawText(X_MID - TW("Dot turns green outside dead-zone", 1.05f) * 0.5f,
            contentY + 268.f, "Dot turns green outside dead-zone", 1.05f, COL_DIM);
    }

    // ── CIRCULARITY test ───────────────────────────────────────────────────
    else if (s_stickTest == 1)
    {
        // Update left stick buckets — primary bucket only, no neighbour smear.
        if (lx != 0 || ly != 0)
        {
            float r = StickRadius(lx, ly);
            if (r > 0.05f)
            {
                int b = AngleBucket(lx, ly);
                if (r > s_circMaxR[b]) s_circMaxR[b] = r;
                s_circHasData = true;
            }
        }
        // Update right stick buckets
        if (rx != 0 || ry != 0)
        {
            float r = StickRadius(rx, ry);
            if (r > 0.05f)
            {
                int b = AngleBucket(rx, ry);
                if (r > s_circMaxRR[b]) s_circMaxRR[b] = r;
                s_circHasDataR = true;
            }
        }

        const float PR = 80.f;
        float lcx = X_MID - 130.f;
        float rcx = X_MID + 130.f;
        float cy = contentY + 110.f;

        // ── Left plot ──
        CircPlot(lcx, cy, PR, s_circMaxR, lx, ly);
        DrawText(lcx - TW("LEFT STICK", 1.15f) * 0.5f, cy - PR - 16.f,
            "LEFT STICK", 1.15f, COL_YELLOW);

        // Coverage: buckets that reached gate (>= 0.7)
        int lcov = 0;
        float lminR = 2.f;
        for (int b = 0; b < CIRC_BUCKETS; ++b)
        {
            if (s_circMaxR[b] >= 0.7f)
            {
                ++lcov;
                if (s_circMaxR[b] < lminR) lminR = s_circMaxR[b];
            }
        }
        {
            float ry2 = cy + PR + 6.f;
            char lcovbuf[8]; IntToStr(lcov * 100 / CIRC_BUCKETS, lcovbuf, sizeof(lcovbuf));
            char lcovstr[12]; StrCopy(lcovstr, sizeof(lcovstr), lcovbuf); StrCat2(lcovstr, sizeof(lcovstr), lcovstr, "%");
            DrawText(lcx - 60.f, ry2, "gate:", 1.1f, COL_GRAY);
            DrawText(lcx - 22.f, ry2, lcovstr, 1.1f, lcov >= CIRC_BUCKETS ? COL_GREEN : COL_ORANGE);
            if (lcov > 0 && lminR <= 1.f)
            {
                int lminPct;
                { float f = lminR * 100.f; __asm { fld f } __asm { fistp lminPct } }
                char lminbuf[8]; IntToStr(lminPct, lminbuf, sizeof(lminbuf));
                char lminstr[12]; StrCopy(lminstr, sizeof(lminstr), lminbuf); StrCat2(lminstr, sizeof(lminstr), lminstr, "%");
                DrawText(lcx + 10.f, ry2, "min R:", 1.1f, COL_GRAY);
                DrawText(lcx + 52.f, ry2, lminstr, 1.1f, lminPct >= 85 ? COL_GREEN : COL_ORANGE);
            }
            if (!s_circHasData)
                DrawText(lcx - TW("Rotate fully", 1.05f) * 0.5f, ry2 + 14.f,
                    "Rotate fully", 1.05f, COL_DIM);
        }

        // ── Right plot ──
        CircPlot(rcx, cy, PR, s_circMaxRR, rx, ry);
        DrawText(rcx - TW("RIGHT STICK", 1.15f) * 0.5f, cy - PR - 16.f,
            "RIGHT STICK", 1.15f, COL_YELLOW);

        int rcov = 0;
        float rminR = 2.f;
        for (int b = 0; b < CIRC_BUCKETS; ++b)
        {
            if (s_circMaxRR[b] >= 0.7f)
            {
                ++rcov;
                if (s_circMaxRR[b] < rminR) rminR = s_circMaxRR[b];
            }
        }
        {
            float ry2 = cy + PR + 6.f;
            char rcovbuf[8]; IntToStr(rcov * 100 / CIRC_BUCKETS, rcovbuf, sizeof(rcovbuf));
            char rcovstr[12]; StrCopy(rcovstr, sizeof(rcovstr), rcovbuf); StrCat2(rcovstr, sizeof(rcovstr), rcovstr, "%");
            DrawText(rcx - 60.f, ry2, "gate:", 1.1f, COL_GRAY);
            DrawText(rcx - 22.f, ry2, rcovstr, 1.1f, rcov >= CIRC_BUCKETS ? COL_GREEN : COL_ORANGE);
            if (rcov > 0 && rminR <= 1.f)
            {
                int rminPct;
                { float f = rminR * 100.f; __asm { fld f } __asm { fistp rminPct } }
                char rminbuf[8]; IntToStr(rminPct, rminbuf, sizeof(rminbuf));
                char rminstr[12]; StrCopy(rminstr, sizeof(rminstr), rminbuf); StrCat2(rminstr, sizeof(rminstr), rminstr, "%");
                DrawText(rcx + 10.f, ry2, "min R:", 1.1f, COL_GRAY);
                DrawText(rcx + 52.f, ry2, rminstr, 1.1f, rminPct >= 85 ? COL_GREEN : COL_ORANGE);
            }
            if (!s_circHasDataR)
                DrawText(rcx - TW("Rotate fully", 1.05f) * 0.5f, ry2 + 14.f,
                    "Rotate fully", 1.05f, COL_DIM);
        }

        // Centre footer legend
        DrawText(X_MID - TW("Green dot = gate reached (R >= 70%)", 1.05f) * 0.5f,
            contentY + 240.f, "Green dot = gate reached (R >= 70%)", 1.05f, COL_DIM);
        DrawText(X_MID - TW("Amber dot = partial contact", 1.05f) * 0.5f,
            contentY + 254.f, "Amber dot = partial contact", 1.05f, D3DCOLOR_XRGB(200, 140, 0));
        DrawText(X_MID - TW("[X] Clear trace", 1.05f) * 0.5f,
            contentY + 268.f, "[X] Clear trace", 1.05f, COL_DIM);
    }

    // ── DRIFT test ─────────────────────────────────────────────────────────
    // Uses raw unfiltered stick values — deadzone filtering would mask real
    // sub-threshold drift and give false OK readings.
    else if (s_stickTest == 2)
    {
        // Warmup: discard first 60 frames so the sticks settle before we measure
        if (s_driftWarmup > 0)
        {
            s_driftWarmup--;
        }
        else
        {
            // Phase 1: accumulate baseline over first BASELINE_FRAMES samples
            if (s_driftBaselineCount < BASELINE_FRAMES)
            {
                s_driftBaselineAccLX += rawLX; s_driftBaselineAccLY += rawLY;
                s_driftBaselineAccRX += rawRX; s_driftBaselineAccRY += rawRY;
                s_driftBaselineCount++;
                if (s_driftBaselineCount == BASELINE_FRAMES)
                {
                    s_driftBaselineLX = s_driftBaselineAccLX / BASELINE_FRAMES;
                    s_driftBaselineLY = s_driftBaselineAccLY / BASELINE_FRAMES;
                    s_driftBaselineRX = s_driftBaselineAccRX / BASELINE_FRAMES;
                    s_driftBaselineRY = s_driftBaselineAccRY / BASELINE_FRAMES;
                }
            }
            else
            {
                // Phase 2: sample with jitter nulling — values within ADC_JITTER
                // of the captured baseline are clamped to baseline so ±1 ADC
                // oscillation doesn't inflate the drift average.
                int sLX = rawLX, sLY = rawLY, sRX = rawRX, sRY = rawRY;
                if (sLX >= s_driftBaselineLX - ADC_JITTER && sLX <= s_driftBaselineLX + ADC_JITTER) sLX = s_driftBaselineLX;
                if (sLY >= s_driftBaselineLY - ADC_JITTER && sLY <= s_driftBaselineLY + ADC_JITTER) sLY = s_driftBaselineLY;
                if (sRX >= s_driftBaselineRX - ADC_JITTER && sRX <= s_driftBaselineRX + ADC_JITTER) sRX = s_driftBaselineRX;
                if (sRY >= s_driftBaselineRY - ADC_JITTER && sRY <= s_driftBaselineRY + ADC_JITTER) sRY = s_driftBaselineRY;

                s_driftLX[s_driftHead] = sLX;
                s_driftLY[s_driftHead] = sLY;
                s_driftRX[s_driftHead] = sRX;
                s_driftRY[s_driftHead] = sRY;
                s_driftHead = (s_driftHead + 1) % DRIFT_SAMPLES;
                if (s_driftCount < DRIFT_SAMPLES) s_driftCount++;
            }
        }

        // Recompute averages
        if (s_driftCount > 0)
        {
            int aLX = 0, aLY = 0, aRX = 0, aRY = 0;
            for (int i = 0; i < s_driftCount; ++i)
            {
                aLX += s_driftLX[i]; aLY += s_driftLY[i];
                aRX += s_driftRX[i]; aRY += s_driftRY[i];
            }
            s_driftAvgLX = aLX / s_driftCount;
            s_driftAvgLY = aLY / s_driftCount;
            s_driftAvgRX = aRX / s_driftCount;
            s_driftAvgRY = aRY / s_driftCount;
        }

        const float PR = 70.f;
        // DRIFT_WARN: 4% of 32767 = ~1310. ADC noise floor is 2-4% so we warn
        // only above that — genuine hardware drift on worn sticks typically
        // starts at 5-15% and causes visible input creep in games.
        const int DRIFT_WARN = 1310;

        // Helper: format axis value + percentage for display
        // e.g. "1234 (3.8%)"
        auto FmtDrift = [](int val, char* buf, int bufLen)
            {
                char vb[10], pb[6];
                IntToStr(val, vb, sizeof(vb));
                // percentage = |val| * 100 / 32767 — one decimal
                int absVal = val < 0 ? -val : val;
                int pct100 = absVal * 1000 / 32767;  // tenths of percent
                int pctInt = pct100 / 10;
                int pctDec = pct100 % 10;
                IntToStr(pctInt, pb, sizeof(pb));
                StrCopy(buf, bufLen, vb);
                StrCat2(buf, bufLen, buf, " (");
                StrCat2(buf, bufLen, buf, pb);
                StrCat2(buf, bufLen, buf, ".");
                char pd[4]; IntToStr(pctDec, pd, sizeof(pd));
                StrCat2(buf, bufLen, buf, pd);
                StrCat2(buf, bufLen, buf, "%)");
            };

        // Left stick plot — dot shows average position
        float lcx = X_MID - 130.f;
        float lcy = contentY + 100.f;
        StickPlot(lcx, lcy, PR, s_driftAvgLX, s_driftAvgLY, 0.f,
            ISqrt(s_driftAvgLX * s_driftAvgLX + s_driftAvgLY * s_driftAvgLY) > DRIFT_WARN
            ? COL_RED : COL_GREEN);

        DrawText(lcx - TW("LEFT STICK", 1.15f) * 0.5f, lcy - PR - 16.f,
            "LEFT STICK", 1.15f, COL_YELLOW);

        char lxb[20], lyb[20];
        FmtDrift(s_driftAvgLX, lxb, sizeof(lxb));
        FmtDrift(s_driftAvgLY, lyb, sizeof(lyb));
        float lblY = lcy + PR + 6.f;
        DrawText(lcx - 50.f, lblY, "avg X:", 1.1f, COL_GRAY);
        DrawText(lcx + 10.f, lblY, lxb, 1.0f,
            (s_driftAvgLX > DRIFT_WARN || s_driftAvgLX < -DRIFT_WARN) ? COL_RED : COL_GREEN);
        DrawText(lcx - 50.f, lblY + 14.f, "avg Y:", 1.1f, COL_GRAY);
        DrawText(lcx + 10.f, lblY + 14.f, lyb, 1.0f,
            (s_driftAvgLY > DRIFT_WARN || s_driftAvgLY < -DRIFT_WARN) ? COL_RED : COL_GREEN);

        // Right stick
        float rcx = X_MID + 130.f;
        float rcy = contentY + 100.f;
        StickPlot(rcx, rcy, PR, s_driftAvgRX, s_driftAvgRY, 0.f,
            ISqrt(s_driftAvgRX * s_driftAvgRX + s_driftAvgRY * s_driftAvgRY) > DRIFT_WARN
            ? COL_RED : COL_GREEN);

        DrawText(rcx - TW("RIGHT STICK", 1.15f) * 0.5f, rcy - PR - 16.f,
            "RIGHT STICK", 1.15f, COL_YELLOW);

        char rxb[20], ryb[20];
        FmtDrift(s_driftAvgRX, rxb, sizeof(rxb));
        FmtDrift(s_driftAvgRY, ryb, sizeof(ryb));
        DrawText(rcx - 50.f, lblY, "avg X:", 1.1f, COL_GRAY);
        DrawText(rcx + 10.f, lblY, rxb, 1.0f,
            (s_driftAvgRX > DRIFT_WARN || s_driftAvgRX < -DRIFT_WARN) ? COL_RED : COL_GREEN);
        DrawText(rcx - 50.f, lblY + 14.f, "avg Y:", 1.1f, COL_GRAY);
        DrawText(rcx + 10.f, lblY + 14.f, ryb, 1.0f,
            (s_driftAvgRY > DRIFT_WARN || s_driftAvgRY < -DRIFT_WARN) ? COL_RED : COL_GREEN);

        // Centre status + instruction
        if (s_driftWarmup > 0 || s_driftBaselineCount < BASELINE_FRAMES)
        {
            // Still settling or capturing baseline
            char cdBuf[4];
            int remaining = s_driftWarmup > 0 ? s_driftWarmup : (BASELINE_FRAMES - s_driftBaselineCount);
            IntToStr(remaining, cdBuf, sizeof(cdBuf));
            const char* phase = s_driftWarmup > 0 ? "Settling... " : "Calibrating... ";
            char cdStr[28];
            StrCopy(cdStr, sizeof(cdStr), phase);
            StrCat2(cdStr, sizeof(cdStr), cdStr, cdBuf);
            DrawText(X_MID - TW(cdStr, 1.3f) * 0.5f, contentY + 240.f,
                cdStr, 1.3f, COL_GRAY);
        }
        else
        {
            bool drifting =
                ISqrt(s_driftAvgLX * s_driftAvgLX + s_driftAvgLY * s_driftAvgLY) > DRIFT_WARN ||
                ISqrt(s_driftAvgRX * s_driftAvgRX + s_driftAvgRY * s_driftAvgRY) > DRIFT_WARN;
            const char* status = drifting ? "DRIFT DETECTED" : "OK";
            DWORD statusC = drifting ? COL_RED : COL_GREEN;
            DrawText(X_MID - TW(status, 1.4f) * 0.5f, contentY + 240.f, status, 1.4f, statusC);
        }

        char sampbuf[8]; IntToStr(s_driftCount, sampbuf, sizeof(sampbuf));
        char sampstr[24];
        StrCopy(sampstr, sizeof(sampstr), "samples: ");
        StrCat2(sampstr, sizeof(sampstr), sampstr, sampbuf);
        DrawText(X_MID - TW(sampstr, 1.05f) * 0.5f, contentY + 258.f,
            sampstr, 1.05f, COL_DIM);

        DrawText(X_MID - TW("Release both sticks and hold still", 1.05f) * 0.5f,
            contentY + 272.f, "Release both sticks and hold still", 1.05f, COL_DIM);
        DrawText(X_MID - TW("[X] Reset samples", 1.05f) * 0.5f,
            contentY + 286.f, "[X] Reset samples", 1.05f, COL_DIM);
    }

    else if (s_stickTest == 3)
    {
        // ── Trigger Dead-Zone card ─────────────────────────────────────────
        // Two large trigger bars spanning most of the content area.
        // Dead-zone band highlighted in red at the bottom of each bar.
        // Min/max recorded values shown. Prompt to squeeze slowly.

        float contentY = B + 36.f;

        // Large bar geometry — wider and taller than the main view trigger bars
        float LBW = 80.f;   // bar width
        float LBH = 240.f;  // bar height
        float lcx = X_MID - 110.f;  // LT center X
        float rcx = X_MID + 110.f;  // RT center X
        float bcy = contentY + 130.f; // bar center Y

        float x0L = lcx - LBW * 0.5f;
        float x1L = lcx + LBW * 0.5f;
        float x0R = rcx - LBW * 0.5f;
        float x1R = rcx + LBW * 0.5f;
        float yTop = bcy - LBH * 0.5f;
        float yBot = bcy + LBH * 0.5f;

        // ── Draw each bar ──────────────────────────────────────────────────

        // ── Draw LT bar ────────────────────────────────────────────────
        {
            float x0 = x0L;
            float x1 = x1L;
            int   val = s_trigLT;
            int   vMin = s_trigMinLT;
            int   vMax = s_trigMaxLT;
            char* lbl = "LT";
            char* dzLbl = "DZ";
            float dzH = LBH * ((float)TRIG_DZ_THRESHOLD / 255.f);
            float fillH = LBH * ((float)val / 255.f);
            char  vStr[6];
            IntToStr(val, vStr, sizeof(vStr));

            FillRect(x0, yTop, x1, yBot, D3DCOLOR_XRGB(10, 14, 32));
            FillRectGrad(x0, yBot - dzH, x1, yBot,
                D3DCOLOR_XRGB(80, 18, 18), D3DCOLOR_XRGB(50, 10, 10));
            HLine(yBot - dzH, x0, x1, D3DCOLOR_XRGB(200, 50, 50));
            DrawText(x0 + (x1 - x0) * 0.5f - TW(dzLbl, 1.0f) * 0.5f,
                yBot - dzH * 0.5f - 5.f, dzLbl, 1.0f, D3DCOLOR_XRGB(200, 80, 80));
            if (fillH > 0.5f)
            {
                DWORD fc = val < 128 ? D3DCOLOR_XRGB(28, 170, 55) :
                    val < 210 ? D3DCOLOR_XRGB(200, 175, 20) :
                    D3DCOLOR_XRGB(210, 55, 30);
                FillRect(x0 + 1.f, yBot - fillH, x1 - 1.f, yBot, fc);
            }
            HLine(yTop + LBH * 0.25f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));
            HLine(yTop + LBH * 0.50f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));
            HLine(yTop + LBH * 0.75f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));
            HLine(yTop, x0, x1, COL_BORDER);
            HLine(yBot, x0, x1, COL_BORDER);
            VLine(x0, yTop, yBot, COL_BORDER);
            VLine(x1, yTop, yBot, COL_BORDER);
            DrawText(x0 + (x1 - x0) * 0.5f - TW(lbl, 1.4f) * 0.5f,
                yTop - 20.f, lbl, 1.4f, COL_YELLOW);
            DrawText(x0 + (x1 - x0) * 0.5f - TW(vStr, 1.2f) * 0.5f,
                yBot + 6.f, vStr, 1.2f, COL_WHITE);
            if (s_trigHasData)
            {
                char mmStr[24];
                char tmp[6];
                StrCopy(mmStr, sizeof(mmStr), "Min:");
                IntToStr(vMin, tmp, sizeof(tmp));
                StrCat2(mmStr, sizeof(mmStr), mmStr, tmp);
                DrawText(x0, yBot + 22.f, mmStr, 1.0f, COL_DIM);
                StrCopy(mmStr, sizeof(mmStr), "Max:");
                IntToStr(vMax, tmp, sizeof(tmp));
                StrCat2(mmStr, sizeof(mmStr), mmStr, tmp);
                DrawText(x0, yBot + 36.f, mmStr, 1.0f, COL_DIM);
                if (vMin <= TRIG_DZ_THRESHOLD && vMax > TRIG_DZ_THRESHOLD)
                {
                    char* w = "PASSES DZ";
                    DrawText(x0 + (x1 - x0) * 0.5f - TW(w, 1.0f) * 0.5f,
                        yBot + 52.f, w, 1.0f, D3DCOLOR_XRGB(28, 200, 28));
                }
                else if (vMax <= TRIG_DZ_THRESHOLD)
                {
                    char* w = "IN DZ";
                    DrawText(x0 + (x1 - x0) * 0.5f - TW(w, 1.0f) * 0.5f,
                        yBot + 52.f, w, 1.0f, D3DCOLOR_XRGB(200, 80, 80));
                }
            }
        }

        // ── Draw RT bar ────────────────────────────────────────────────
        {
            float x0 = x0R;
            float x1 = x1R;
            int   val = s_trigRT;
            int   vMin = s_trigMinRT;
            int   vMax = s_trigMaxRT;
            char* lbl = "RT";
            char* dzLbl = "DZ";
            float dzH = LBH * ((float)TRIG_DZ_THRESHOLD / 255.f);
            float fillH = LBH * ((float)val / 255.f);
            char  vStr[6];
            IntToStr(val, vStr, sizeof(vStr));

            FillRect(x0, yTop, x1, yBot, D3DCOLOR_XRGB(10, 14, 32));
            FillRectGrad(x0, yBot - dzH, x1, yBot,
                D3DCOLOR_XRGB(80, 18, 18), D3DCOLOR_XRGB(50, 10, 10));
            HLine(yBot - dzH, x0, x1, D3DCOLOR_XRGB(200, 50, 50));
            DrawText(x0 + (x1 - x0) * 0.5f - TW(dzLbl, 1.0f) * 0.5f,
                yBot - dzH * 0.5f - 5.f, dzLbl, 1.0f, D3DCOLOR_XRGB(200, 80, 80));
            if (fillH > 0.5f)
            {
                DWORD fc = val < 128 ? D3DCOLOR_XRGB(28, 170, 55) :
                    val < 210 ? D3DCOLOR_XRGB(200, 175, 20) :
                    D3DCOLOR_XRGB(210, 55, 30);
                FillRect(x0 + 1.f, yBot - fillH, x1 - 1.f, yBot, fc);
            }
            HLine(yTop + LBH * 0.25f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));
            HLine(yTop + LBH * 0.50f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));
            HLine(yTop + LBH * 0.75f, x0, x1, D3DCOLOR_XRGB(25, 35, 65));
            HLine(yTop, x0, x1, COL_BORDER);
            HLine(yBot, x0, x1, COL_BORDER);
            VLine(x0, yTop, yBot, COL_BORDER);
            VLine(x1, yTop, yBot, COL_BORDER);
            DrawText(x0 + (x1 - x0) * 0.5f - TW(lbl, 1.4f) * 0.5f,
                yTop - 20.f, lbl, 1.4f, COL_YELLOW);
            DrawText(x0 + (x1 - x0) * 0.5f - TW(vStr, 1.2f) * 0.5f,
                yBot + 6.f, vStr, 1.2f, COL_WHITE);
            if (s_trigHasData)
            {
                char mmStr[24];
                char tmp[6];
                StrCopy(mmStr, sizeof(mmStr), "Min:");
                IntToStr(vMin, tmp, sizeof(tmp));
                StrCat2(mmStr, sizeof(mmStr), mmStr, tmp);
                DrawText(x0, yBot + 22.f, mmStr, 1.0f, COL_DIM);
                StrCopy(mmStr, sizeof(mmStr), "Max:");
                IntToStr(vMax, tmp, sizeof(tmp));
                StrCat2(mmStr, sizeof(mmStr), mmStr, tmp);
                DrawText(x0, yBot + 36.f, mmStr, 1.0f, COL_DIM);
                if (vMin <= TRIG_DZ_THRESHOLD && vMax > TRIG_DZ_THRESHOLD)
                {
                    char* w = "PASSES DZ";
                    DrawText(x0 + (x1 - x0) * 0.5f - TW(w, 1.0f) * 0.5f,
                        yBot + 52.f, w, 1.0f, D3DCOLOR_XRGB(28, 200, 28));
                }
                else if (vMax <= TRIG_DZ_THRESHOLD)
                {
                    char* w = "IN DZ";
                    DrawText(x0 + (x1 - x0) * 0.5f - TW(w, 1.0f) * 0.5f,
                        yBot + 52.f, w, 1.0f, D3DCOLOR_XRGB(200, 80, 80));
                }
            }
        }

        // Centre info
        char dzValStr[24];
        char tmp2[6];
        DrawText(X_MID - TW("Squeeze triggers slowly from rest", 1.05f) * 0.5f,
            contentY + 272.f, "Squeeze triggers slowly from rest", 1.05f, COL_DIM);
        DrawText(X_MID - TW("[X] Reset stats", 1.05f) * 0.5f,
            contentY + 286.f, "[X] Reset stats", 1.05f, COL_DIM);

        // Dead-zone threshold label at centre
        StrCopy(dzValStr, sizeof(dzValStr), "Dead-zone threshold: ");
        IntToStr(TRIG_DZ_THRESHOLD, tmp2, sizeof(tmp2));
        StrCat2(dzValStr, sizeof(dzValStr), dzValStr, tmp2);
        DrawText(X_MID - TW(dzValStr, 1.0f) * 0.5f,
            contentY + 300.f, dzValStr, 1.0f, D3DCOLOR_XRGB(200, 80, 80));
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
        "[START+A] Rumble   [START+Up] Stick Test   [START+B] Exit");

    // =========================================================
    // PORT STATUS STRIP  — centered, above trigger bars
    // Four boxes: green=connected, dark=disconnected
    // =========================================================
    {
        const float PBW = 46.f;   // box width
        const float PBH = 20.f;   // box height
        const float PGAP = 6.f;    // gap between boxes
        const float totalW = PBW * 4.f + PGAP * 3.f;
        float px = X_MID - totalW * 0.5f;
        float py = B + 8.f;

        const char* portLabels[4] = { "1", "2", "3", "4" };
        for (int p = 0; p < 4; ++p)
        {
            bool conn = IsPortConnected(p);
            float x0 = px + (PBW + PGAP) * (float)p;
            float x1 = x0 + PBW;
            float y0 = py;
            float y1 = py + PBH;
            float cx = (x0 + x1) * 0.5f;
            float cy = (y0 + y1) * 0.5f;

            DWORD fill = conn ? D3DCOLOR_XRGB(10, 40, 18) : D3DCOLOR_XRGB(10, 14, 32);
            DWORD border = conn ? D3DCOLOR_XRGB(80, 255, 110) : COL_BORDER;
            DWORD tc = conn ? COL_GREEN : COL_DIM;

            FillRect(x0, y0, x1, y1, fill);
            HLine(y0, x0, x1, border);
            HLine(y1, x0, x1, border);
            VLine(x0, y0, y1, border);
            VLine(x1, y0, y1, border);
            DrawText(cx - TW(portLabels[p], 1.2f) * 0.5f, cy - 5.f,
                portLabels[p], 1.2f, tc);
        }

        // Disconnects row — label on the left, count centred under each box
        float discY = py + PBH + 4.f;
        DrawText(px - 84.f, discY, "Disconnects:", 1.1f, COL_GRAY);
        for (int p = 0; p < 4; ++p)
        {
            float cx = px + (PBW + PGAP) * (float)p + PBW * 0.5f;
            char buf[8];
            IntToStr(s_discCount[p], buf, sizeof(buf));
            DWORD cc = s_discCount[p] > 0 ? COL_RED : COL_DIM;
            DrawText(cx - TW(buf, 1.2f) * 0.5f, discY, buf, 1.2f, cc);
        }
    }
    // LT left, RT right
    // =========================================================

    TrigBar(X_LEFT, B + Y_TRIG, "LT", lt);
    TrigBar(X_RIGHT, B + Y_TRIG, "RT", rt);

    // =========================================================
    // META BUTTONS  — BACK/START (center-left), BLACK/WHITE (center-right)
    // =========================================================

    float metaY = B + Y_META;

    // BACK and START centered around X_MID - 90
    float metaLCX = X_MID - 90.f;
    Box(metaLCX - WBW * 0.5f - 4.f, metaY,
        WBW, WBH, "BACK", 1.1f, (cur & BTN_BACK) != 0);
    Box(metaLCX + WBW * 0.5f + 4.f, metaY,
        WBW, WBH, "START", 1.1f, (cur & BTN_START) != 0);

    // WHITE (left) and BLACK (right) — matches physical controller layout
    float metaRCX = X_MID + 90.f;
    float whtCX = metaRCX - WBW * 0.5f - 4.f;
    float blkCX = metaRCX + WBW * 0.5f + 4.f;
    Box(whtCX, metaY, WBW, WBH, "WHT", 1.1f, (cur & BTN_WHITE) != 0);
    Box(blkCX, metaY, WBW, WBH, "BLK", 1.1f, (cur & BTN_BLACK) != 0);

    // Analog value readout below WHITE/BLACK
    {
        char bv[5], wv[5];
        IntToStr(blk, bv, sizeof(bv));
        IntToStr(wht, wv, sizeof(wv));
        DWORD bc = blk > 30 ? COL_CYAN : COL_DIM;
        DWORD wc = wht > 30 ? COL_CYAN : COL_DIM;
        DrawText(whtCX - TW(wv, 1.05f) * 0.5f, metaY + WBH * 0.5f + 4.f, wv, 1.05f, wc);
        DrawText(blkCX - TW(bv, 1.05f) * 0.5f, metaY + WBH * 0.5f + 4.f, bv, 1.05f, bc);
    }

    // =========================================================
    // MEMORY UNITS — center column, between meta and stick rows
    // Four ports, each showing slot A (top) and slot B (bottom).
    // Presence only — no data read.
    // =========================================================
    {
        const char* hdr = "MEMORY UNITS";
        float muHdrY = B + Y_META + 34.f;
        DrawText(X_MID - TW(hdr, 1.1f) * 0.5f, muHdrY, hdr, 1.1f, COL_YELLOW);

        // Four port columns, centred at X_MID
        const float MW = 44.f;   // column width
        const float MG = 8.f;    // gap between columns
        const float totalMW = MW * 4.f + MG * 3.f;
        float mx = X_MID - totalMW * 0.5f;
        float muY = muHdrY + LINE_H + 4.f;

        // Port number header row
        const char* pLabels[4] = { "P1", "P2", "P3", "P4" };
        for (int p = 0; p < 4; ++p)
        {
            float cx = mx + (MW + MG) * (float)p + MW * 0.5f;
            DrawText(cx - TW(pLabels[p], 1.0f) * 0.5f, muY, pLabels[p], 1.0f, COL_GRAY);
        }
        muY += LINE_H + 2.f;

        // Slot rows
        const char* slotNames[2] = { "Slot A", "Slot B" };
        for (int s = 0; s < 2; ++s)
        {
            DrawText(mx - 52.f, muY, slotNames[s], 1.0f, COL_GRAY);
            for (int p = 0; p < 4; ++p)
            {
                float cx = mx + (MW + MG) * (float)p + MW * 0.5f;
                bool present = IsMUPresent(p, s);
                const char* state = present ? "PRESENT" : "---";
                DWORD sc = present ? COL_GREEN : COL_DIM;
                DrawText(cx - TW(state, 1.0f) * 0.5f, muY, state, 1.0f, sc);
            }
            muY += LINE_H + 1.f;
        }
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

    // Raw (unfiltered) values for the drift test — deadzone would mask real drift
    int rawLX, rawLY, rawRX, rawRY;
    GetRawSticks(rawLX, rawLY, rawRX, rawRY);

    int lt, rt, blk, wht, btnA, btnB, btnX, btnY;
    GetTriggers(lt, rt, blk, wht, btnA, btnB, btnX, btnY);

    // ── Per-port disconnect tracking ────────────────────────────────────────
    // Increment counter each time a connected port goes away.
    for (int p = 0; p < 4; ++p)
    {
        bool conn = IsPortConnected(p);
        if (s_wasConn[p] && !conn)
            s_discCount[p]++;
        s_wasConn[p] = conn;
    }

    // ── Stick test card ─────────────────────────────────────────────────────
    if (s_stickMode)
    {
        if (Edge(cur, s_prev, BTN_B))
        {
            s_stickMode = false;
            s_prev = cur;
            return;
        }
        if (Edge(cur, s_prev, BTN_DPAD_RIGHT))
            s_stickTest = (s_stickTest + 1) % 4;
        if (Edge(cur, s_prev, BTN_DPAD_LEFT))
            s_stickTest = (s_stickTest + 3) % 4;
        if (Edge(cur, s_prev, BTN_X))
        {
            if (s_stickTest == 1)
            {
                // Clear circularity trace
                for (int i = 0; i < CIRC_BUCKETS; ++i) { s_circMaxR[i] = 0.f; s_circMaxRR[i] = 0.f; }
                s_circHasData = false;
                s_circHasDataR = false;
            }
            else if (s_stickTest == 2)
            {
                // Reset drift samples
                s_driftHead = 0; s_driftCount = 0; s_driftWarmup = 60; s_driftBaselineCount = 0; s_driftBaselineAccLX = 0; s_driftBaselineAccLY = 0; s_driftBaselineAccRX = 0; s_driftBaselineAccRY = 0;
                s_driftAvgLX = 0; s_driftAvgLY = 0;
                s_driftAvgRX = 0; s_driftAvgRY = 0; s_driftBaselineLX = 0; s_driftBaselineLY = 0; s_driftBaselineRX = 0; s_driftBaselineRY = 0;
            }
            else if (s_stickTest == 3)
            {
                // Reset trigger dead-zone stats
                s_trigMinLT = 255; s_trigMaxLT = 0;
                s_trigMinRT = 255; s_trigMaxRT = 0;
                s_trigHasData = false;
            }
        }
        // Update trigger dead-zone card live values
        if (s_stickTest == 3)
        {
            s_trigLT = lt;
            s_trigRT = rt;
            if (s_trigLT > 0 || s_trigRT > 0) s_trigHasData = true;
            if (s_trigLT < s_trigMinLT) s_trigMinLT = s_trigLT;
            if (s_trigLT > s_trigMaxLT) s_trigMaxLT = s_trigLT;
            if (s_trigRT < s_trigMinRT) s_trigMinRT = s_trigRT;
            if (s_trigRT > s_trigMaxRT) s_trigMaxRT = s_trigRT;
        }
        s_prev = cur;
        RenderStickTest(logo, lx, ly, rx, ry, rawLX, rawLY, rawRX, rawRY);
        return;
    }

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

    // START+DPAD_UP -> enter stick test card
    if ((cur & BTN_START) && Edge(cur, s_prev, BTN_DPAD_UP))
    {
        s_stickMode = true;
        s_stickTest = 0;
        s_circHasData = false;
        s_circHasDataR = false;
        for (int i = 0; i < CIRC_BUCKETS; ++i) { s_circMaxR[i] = 0.f; s_circMaxRR[i] = 0.f; }
        s_driftHead = 0; s_driftCount = 0; s_driftWarmup = 60; s_driftBaselineCount = 0; s_driftBaselineAccLX = 0; s_driftBaselineAccLY = 0; s_driftBaselineAccRX = 0; s_driftBaselineAccRY = 0;
        s_driftAvgLX = 0; s_driftAvgLY = 0;
        s_driftAvgRX = 0; s_driftAvgRY = 0; s_driftBaselineLX = 0; s_driftBaselineLY = 0; s_driftBaselineRX = 0; s_driftBaselineRY = 0;
        s_trigMinLT = 255; s_trigMaxLT = 0;
        s_trigMinRT = 255; s_trigMaxRT = 0;
        s_trigHasData = false;
        s_prev = cur;
        return;
    }

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
// ============================================================================
// AutoRun — detect connected controllers, report port status
// ============================================================================

void ControllerTest_AutoRun(HANDLE hReport)
{
    // Pump a few frames to let XGetDeviceChanges settle
    for (int i = 0; i < 10; ++i) { PumpInput(); Sleep(16); }

    char line[64]; DWORD w;
    int found = 0;

    for (int port = 0; port < 4; ++port)
    {
        char portCh[4]; IntToStr(port + 1, portCh, sizeof(portCh));
        bool conn = IsPortConnected(port);

        StrCopy(line, sizeof(line), "Port ");
        StrCat2(line, sizeof(line), line, portCh);
        StrCat2(line, sizeof(line), line, ":         ");
        StrCat2(line, sizeof(line), line, conn ? "Controller connected" : "Empty");
        StrCat2(line, sizeof(line), line, "\r\n");
        WriteFile(hReport, line, StrLen(line), &w, NULL);

        if (conn) ++found;
    }

    char tot[4]; IntToStr(found, tot, sizeof(tot));
    StrCopy(line, sizeof(line), "Total found:  ");
    StrCat2(line, sizeof(line), line, tot);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hReport, line, StrLen(line), &w, NULL);
}