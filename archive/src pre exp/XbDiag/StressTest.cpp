// StressTest.cpp
// XbDiag - Stress Test module.
//
// Card 0: CPU stress test (Prime95-style FPU torture)
// Card 1: RAM stress test (stub)
//
// ===========================================================================
// Layout (640x480 design space)
// ===========================================================================
//
//  TOP_BAR_H = 58   CONTENT_Y = 58   BOT_BAR_Y = 450   usable H = 392
//
//  [TOP BAR - DrawPageChrome]                              Y=0..58
//  Tab strip  CPU | RAM                                    Y=58..74   H=16
//  ─────────────────────────────────────────────────────────────────
//  Three info panels side by side                          Y=75..145  H=70
//    [CPU TEMP | FAN SPEED | CPU SPEED]
//  CPU LOAD label + bar                                    Y=147..170 H=23
//  Graph header rule                                       Y=172
//  Graph (scrolling telemetry)                             Y=174..418 H=244
//  Abort hold bar (running only, overlays above bot bar)   Y=436..442
//  [BOT BAR - DrawPageChrome]                              Y=450..480
//
// ===========================================================================
// CPU stress kernel
// ===========================================================================
//
// Coppermine Pentium III (733MHz) has:
//   L1 cache: 32KB data
//   L2 cache: 256KB (on-die, half-speed)
//   FPU:      fully pipelined, 1 fmul + 1 fadd per clock (dual-issue)
//
// Strategy: keep a 4KB float array (fits entirely in L1) and hammer it with
// fmul+fadd pairs every iteration.  This is what Prime95 small-FFT mode does
// on this class of CPU — it's the FPU that generates heat, not integer work.
//
// The kernel runs from after Present until the next frame's input poll,
// so it occupies the full inter-frame gap.  We do NOT cap it to 20ms —
// we spin the entire time the test is running, yielding only for the
// DrawPageChrome + Present call (a few hundred µs at most).
//
// Load % reported: always 100 while RUNNING (we are saturating the FPU).
//
// ===========================================================================
// Abort UX
// ===========================================================================
//
// Hold [Back]+[A] simultaneously for 5 seconds.
// A red fill bar + countdown label appears during the hold.
// Releasing before 5s cancels. Full hold -> SSTATE_IDLE.
//
// ===========================================================================

#include "StressTest.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

// ============================================================================
// Config
// ============================================================================

static const int   HISTORY_LEN = 128;
static const DWORD SAMPLE_INTERVAL_MS = 500;
static const DWORD ABORT_HOLD_MS = 5000;

// CPU temp thresholds
static const int CPU_WARN = 65;
static const int CPU_HOT = 80;
static const int GRAPH_MAX = 100;

// FPU stress working set — 4KB, guaranteed fits in Coppermine L1 (32KB)
static const int FPU_WORK_FLOATS = 1024;  // 1024 * 4 = 4096 bytes

// ============================================================================
// Layout (all Y values are in 640x480 design space)
// ============================================================================

// Tab strip — sits flush at CONTENT_Y
static const float TAB_Y = CONTENT_Y;       // 58
static const float TAB_H = 16.f;
static const float TAB_W = 56.f;
static const float TAB_CPU_X = LM;             // 32
static const float TAB_RAM_X = LM + TAB_W + 4.f;  // 92

// Info panels — immediately below tab strip
static const float PANEL_TOP = TAB_Y + TAB_H + 1.f;  // 75
static const float PANEL_H = 70.f;
static const float PANEL_W = 182.f;
static const float PANEL_GAP = 6.f;
static const float PANEL_T_X = LM;                            // CPU TEMP
static const float PANEL_F_X = PANEL_T_X + PANEL_W + PANEL_GAP; // FAN
static const float PANEL_M_X = PANEL_F_X + PANEL_W + PANEL_GAP; // MHz

// Inside panel
static const float P_LBL_DY = 4.f;    // label from panel top
static const float P_BIG_DY = 16.f;   // big value from panel top
static const float P_BAR_DY = 50.f;   // gauge bar from panel top
static const float P_BAR_H = 12.f;
static const float P_BIG_SC = 2.8f;
static const float P_PAD_X = 8.f;

// Load bar
static const float LOAD_LBL_Y = PANEL_TOP + PANEL_H + 5.f;    // 150
static const float LOAD_BAR_Y = LOAD_LBL_Y + 13.f;             // 163
static const float LOAD_BAR_H = 14.f;
static const float LOAD_BAR_X = LM;
static const float LOAD_BAR_W = SW - LM * 2.f;                 // 576

// Graph
static const float GRAPH_HDR_Y = LOAD_BAR_Y + LOAD_BAR_H + 5.f;  // 182
static const float GRAPH_X = LM + 28.f;                        // 60
static const float GRAPH_Y = GRAPH_HDR_Y + 13.f;              // 195
static const float GRAPH_W = SW - GRAPH_X - LM - 2.f;         // 546
static const float GRAPH_H = BOT_BAR_Y - GRAPH_Y - 34.f;      // 221
static const float GRAPH_R = GRAPH_X + GRAPH_W;
static const float GRAPH_B = GRAPH_Y + GRAPH_H;

// Abort bar (just above bot bar)
static const float ABORT_BAR_Y = BOT_BAR_Y - 12.f;   // 438
static const float ABORT_BAR_H = 6.f;

// ============================================================================
// State
// ============================================================================

enum StressState { SSTATE_IDLE = 0, SSTATE_CONFIRM, SSTATE_RUNNING };
enum StressCard { CARD_CPU = 0, CARD_RAM = 1 };

static StressState s_state = SSTATE_IDLE;
static StressCard  s_card = CARD_CPU;
static WORD        s_prevBtns = 0;

// Current sensor readings
static BYTE  s_curCPU = 0;
static BYTE  s_curFan = 0;   // raw 0-50 from PIC; display = val*2 %
static int   s_curMHz = 733;
static bool  s_sensorOK = false;
static bool  s_fanOK = false;

// Sensor path
enum SensorPath { PATH_UNKNOWN = 0, PATH_ADM1032, PATH_PIC_16 };
static SensorPath s_path = PATH_UNKNOWN;

// PIC averaging
static int s_avg_cpu_acc = 0;
static int s_avg_count = 0;
static const int AVG_SAMPLES = 10;

// Ring buffers
static BYTE s_histCPU[HISTORY_LEN];
static BYTE s_histLoad[HISTORY_LEN];
static BYTE s_histFan[HISTORY_LEN];
static int  s_histHead = 0;
static int  s_histCount = 0;

static DWORD s_lastSample = 0;
static DWORD s_testStartMs = 0;

// Abort hold
static bool  s_abortHolding = false;
static DWORD s_abortHoldStart = 0;

// FPU stress working array — static so it's never on the stack
// Initialised to non-zero in OnEnter so the FPU doesn't short-circuit on 0.0f
static float s_fpuWork[FPU_WORK_FLOATS];

// ============================================================================
// Helpers
// ============================================================================

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

static DWORD TempColor(int v, int warn, int hot)
{
    if (v >= hot)  return COL_RED;
    if (v >= warn) return COL_ORANGE;
    return COL_GREEN;
}

static int HistIdx(int fromOldest)
{
    return (s_histHead + fromOldest) % HISTORY_LEN;
}

static void PushHistory(BYTE cpu, BYTE load, BYTE fan)
{
    int wi;
    if (s_histCount < HISTORY_LEN)
    {
        wi = (s_histHead + s_histCount) % HISTORY_LEN;
        ++s_histCount;
    }
    else
    {
        wi = s_histHead;
        s_histHead = (s_histHead + 1) % HISTORY_LEN;
    }
    s_histCPU[wi] = cpu;
    s_histLoad[wi] = load;
    s_histFan[wi] = fan;
}

// ============================================================================
// CPU MHz via ICS clock generator (SMBus 0xD2, reg 0x09 bits[2:0])
// Ref: Xbox BIOS / modchip documentation. Falls back to 733 on NAK.
// ============================================================================

static int ReadCPUMHz()
{
    BYTE reg = 0;
    if (!SMBusRead(SMBADDR_ICS, 0x09, reg))
        return 733;
    switch (reg & 0x07)
    {
    case 0: return 733;
    case 1: return 800;
    case 2: return 853;
    default: return 733;
    }
}

// ============================================================================
// Sensor sample — mirrors TempMonitor TakeSample, CPU die only
// ============================================================================

static void TakeSample()
{
    BYTE cpu = 0;
    bool ok = false;

    if (s_path == PATH_UNKNOWN)
    {
        BYTE p = 0;
        s_path = SMBusRead(SMBADDR_ADM1032, 0x00, p)
            ? PATH_ADM1032 : PATH_PIC_16;
    }

    if (s_path == PATH_ADM1032)
    {
        BYTE frac = 0;
        ok = SMBusRead(SMBADDR_ADM1032, 0x01, cpu);
        if (ok) SMBusRead(SMBADDR_ADM1032, 0x10, frac);
    }
    else
    {
        BYTE picCPU = 0;
        ok = SMBusRead(SMBADDR_PIC, 0x09, picCPU);
        if (ok)
        {
            s_avg_cpu_acc += (int)picCPU;
            ++s_avg_count;
            if (s_avg_count < AVG_SAMPLES)
                return;
            cpu = (BYTE)(s_avg_cpu_acc / AVG_SAMPLES);
            s_avg_cpu_acc = 0;
            s_avg_count = 0;
        }
    }

    s_sensorOK = ok;
    if (!ok) return;

    s_curCPU = cpu;

    BYTE fanRaw = 0;
    if (SMBusRead(SMBADDR_PIC, 0x10, fanRaw))
    {
        s_curFan = fanRaw;
        s_fanOK = true;
    }

    BYTE load = (s_state == SSTATE_RUNNING) ? 100 : 0;
    BYTE fanPct = s_fanOK ? (BYTE)((int)s_curFan * 2) : 0;
    PushHistory(cpu, load, fanPct);
}

// ============================================================================
// FPU stress kernel — Coppermine P3 Prime95-style small-FFT equivalent.
//
// Keeps 4KB of floats entirely in L1 cache (32KB on Coppermine).
// Each iteration issues one fmul + one fadd per element — the Coppermine FPU
// can dual-issue these, keeping both execution units saturated.
//
// We run until GetTickCount() advances to the deadline passed in.
// The caller sets deadline = now + (frame_budget - render_ms) so the
// CPU stays fully loaded between frames with only the Present call as idle.
//
// The multiply constant (1.00001f / 0.99999f) is chosen so values remain
// finite indefinitely — no NaN or Inf accumulation over a long run.
// ============================================================================

static void FPUStress(DWORD deadline)
{
    int   n = FPU_WORK_FLOATS;
    float* w = s_fpuWork;

    // Keep looping until we've burned through to the deadline.
    // Inner loop: fmul + fadd on adjacent pairs — fills both FPU pipelines.
    while (GetTickCount() < deadline)
    {
        for (int i = 0; i < n - 1; ++i)
        {
            w[i] = w[i] * 1.00001f + w[i + 1] * 0.99999f;
        }
        // Fold last element back to prevent values drifting to zero
        w[n - 1] = w[0] * 1.00001f + w[n - 2] * 0.99999f;
    }
}

// ============================================================================
// OnEnter
// ============================================================================

void StressTest_OnEnter()
{
    s_prevBtns = GetButtons();
    s_state = SSTATE_IDLE;
    s_card = CARD_CPU;
    s_histHead = 0;
    s_histCount = 0;
    s_curCPU = 0;
    s_curFan = 0;
    s_curMHz = 733;
    s_sensorOK = false;
    s_fanOK = false;
    s_path = PATH_UNKNOWN;
    s_avg_cpu_acc = 0;
    s_avg_count = 0;
    s_abortHolding = false;
    s_abortHoldStart = 0;
    s_lastSample = GetTickCount() - SAMPLE_INTERVAL_MS;

    // Seed FPU work array with non-trivial values so first pass is real work
    for (int i = 0; i < FPU_WORK_FLOATS; ++i)
        s_fpuWork[i] = 1.0f + (float)(i & 0xFF) * 0.003921f; // 0..1 spread
}

// ============================================================================
// DrawInfoPanel — compact panel with large readout + tri-color gauge bar
// val and thresholds are 0-100
// ============================================================================

static void DrawInfoPanel(float px, float py,
    const char* title,
    int val, int warnT, int hotT,
    const char* suffix, bool ok)
{
    // Background + border
    FillRectGrad(px, py, px + PANEL_W, py + PANEL_H,
        D3DCOLOR_XRGB(16, 20, 50), D3DCOLOR_XRGB(10, 13, 34));
    HLine(py, px, px + PANEL_W, COL_CYAN);
    HLine(py + PANEL_H, px, px + PANEL_W, COL_BORDER);
    VLine(px, py, py + PANEL_H, COL_BORDER);
    VLine(px + PANEL_W, py, py + PANEL_H, COL_BORDER);

    DrawText(px + P_PAD_X, py + P_LBL_DY, title, 1.1f, COL_YELLOW);

    if (!ok)
    {
        DrawText(px + P_PAD_X, py + P_BIG_DY + 6.f, "ERR", 2.0f, COL_RED);
        return;
    }

    DWORD vc = TempColor(val, warnT, hotT);

    char numBuf[12];  IntToStr(val, numBuf, sizeof(numBuf));
    char disp[16];    StrCat2(disp, sizeof(disp), numBuf, suffix);
    DrawText(px + P_PAD_X, py + P_BIG_DY, disp, P_BIG_SC, vc);

    // Gauge bar
    float bx = px + P_PAD_X;
    float by = py + P_BAR_DY;
    float bw = PANEL_W - P_PAD_X * 2.f;
    FillRect(bx, by, bx + bw, by + P_BAR_H, D3DCOLOR_XRGB(14, 17, 38));

    float fill = (float)val / 100.f; if (fill > 1.f) fill = 1.f;
    float warnF = (float)warnT / 100.f;
    float hotF = (float)hotT / 100.f;
    float fillX = bx + bw * fill;
    float warnX = bx + bw * warnF;
    float hotX = bx + bw * hotF;

    if (fill > 0.f)
    {
        if (fill <= warnF)
        {
            FillRectGrad(bx, by, fillX, by + P_BAR_H,
                D3DCOLOR_XRGB(40, 200, 60), D3DCOLOR_XRGB(20, 120, 30));
        }
        else if (fill <= hotF)
        {
            FillRectGrad(bx, by, warnX, by + P_BAR_H,
                D3DCOLOR_XRGB(40, 200, 60), D3DCOLOR_XRGB(20, 120, 30));
            FillRectGrad(warnX, by, fillX, by + P_BAR_H,
                D3DCOLOR_XRGB(220, 160, 20), D3DCOLOR_XRGB(180, 100, 10));
        }
        else
        {
            FillRectGrad(bx, by, warnX, by + P_BAR_H,
                D3DCOLOR_XRGB(40, 200, 60), D3DCOLOR_XRGB(20, 120, 30));
            FillRectGrad(warnX, by, hotX, by + P_BAR_H,
                D3DCOLOR_XRGB(220, 160, 20), D3DCOLOR_XRGB(180, 100, 10));
            FillRectGrad(hotX, by, fillX, by + P_BAR_H,
                D3DCOLOR_XRGB(255, 60, 30), D3DCOLOR_XRGB(180, 20, 10));
        }
    }

    // Threshold tick marks (only if warn < hot, i.e. normal orientation)
    if (warnT > 0 && warnT < hotT)
    {
        VLine(warnX, by - 2.f, by + P_BAR_H + 2.f, COL_ORANGE);
        VLine(hotX, by - 2.f, by + P_BAR_H + 2.f, COL_RED);
    }

    // Bar border
    HLine(by, bx, bx + bw, COL_BORDER);
    HLine(by + P_BAR_H, bx, bx + bw, COL_BORDER);
    VLine(bx, by, by + P_BAR_H, COL_BORDER);
    VLine(bx + bw, by, by + P_BAR_H, COL_BORDER);
}

// ============================================================================
// DrawGraph — three-line scrolling history (CPU temp, load %, fan %)
// ============================================================================

static void DrawGraph()
{
    // Background
    FillRect(GRAPH_X, GRAPH_Y, GRAPH_R, GRAPH_B, D3DCOLOR_XRGB(10, 13, 30));

    // Danger / warm zone shading
    float dangerY = GRAPH_Y + GRAPH_H * (float)(GRAPH_MAX - CPU_HOT) / GRAPH_MAX;
    float warnY = GRAPH_Y + GRAPH_H * (float)(GRAPH_MAX - CPU_WARN) / GRAPH_MAX;
    FillRect(GRAPH_X, GRAPH_Y, GRAPH_R, dangerY, D3DCOLOR_XRGB(40, 10, 10));
    FillRect(GRAPH_X, dangerY, GRAPH_R, warnY, D3DCOLOR_XRGB(28, 18, 8));

    // Grid lines every 10 units
    for (int t = 10; t < GRAPH_MAX; t += 10)
    {
        float gy = GRAPH_B - GRAPH_H * ((float)t / GRAPH_MAX);
        DWORD gc = (t == CPU_HOT) ? D3DCOLOR_XRGB(100, 30, 30)
            : (t == CPU_WARN) ? D3DCOLOR_XRGB(80, 60, 20)
            : D3DCOLOR_XRGB(25, 30, 55);
        HLine(gy, GRAPH_X, GRAPH_R, gc);

        char lbl[6]; IntToStr(t, lbl, sizeof(lbl));
        DrawText(GRAPH_X - 24.f, gy - LINE_H * 0.5f, lbl, 0.95f,
            (t == CPU_HOT) ? COL_RED :
            (t == CPU_WARN) ? COL_ORANGE : COL_DIM);
    }

    // Border
    HLine(GRAPH_Y, GRAPH_X, GRAPH_R, COL_BORDER);
    HLine(GRAPH_B, GRAPH_X, GRAPH_R, COL_BORDER);
    VLine(GRAPH_X, GRAPH_Y, GRAPH_B, COL_BORDER);
    VLine(GRAPH_R, GRAPH_Y, GRAPH_B, COL_BORDER);

    if (s_histCount < 2) return;

    float xStep = GRAPH_W / (float)(HISTORY_LEN - 1);

    for (int i = 0; i < s_histCount - 1; ++i)
    {
        int idxA = HistIdx(i);
        int idxB = HistIdx(i + 1);

        float xA = GRAPH_R - (float)(s_histCount - 1 - i) * xStep;
        float xB = GRAPH_R - (float)(s_histCount - 2 - i) * xStep;
        if (xA < GRAPH_X) xA = GRAPH_X;
        if (xB < GRAPH_X) xB = GRAPH_X;

        int steps = Ftoi(xB - xA);
        if (steps < 1) steps = 1;

        // CPU Temp — yellow
        {
            float yA = GRAPH_B - GRAPH_H * ((float)s_histCPU[idxA] / GRAPH_MAX);
            float yB = GRAPH_B - GRAPH_H * ((float)s_histCPU[idxB] / GRAPH_MAX);
            for (int s = 0; s <= steps; ++s)
            {
                float t2 = (float)s / steps;
                float lx = xA + (xB - xA) * t2;
                float ly = yA + (yB - yA) * t2;
                if (lx >= GRAPH_X && lx <= GRAPH_R && ly >= GRAPH_Y && ly <= GRAPH_B)
                    FillRect(lx - 1.f, ly - 1.f, lx + 1.f, ly + 1.f, COL_YELLOW);
            }
        }

        // Load — bright green
        {
            DWORD lc = D3DCOLOR_XRGB(50, 255, 80);
            float yA = GRAPH_B - GRAPH_H * ((float)s_histLoad[idxA] / GRAPH_MAX);
            float yB = GRAPH_B - GRAPH_H * ((float)s_histLoad[idxB] / GRAPH_MAX);
            for (int s = 0; s <= steps; ++s)
            {
                float t2 = (float)s / steps;
                float lx = xA + (xB - xA) * t2;
                float ly = yA + (yB - yA) * t2;
                if (lx >= GRAPH_X && lx <= GRAPH_R && ly >= GRAPH_Y && ly <= GRAPH_B)
                    FillRect(lx - 1.f, ly - 1.f, lx + 1.f, ly + 1.f, lc);
            }
        }

        // Fan — cyan
        {
            float yA = GRAPH_B - GRAPH_H * ((float)s_histFan[idxA] / GRAPH_MAX);
            float yB = GRAPH_B - GRAPH_H * ((float)s_histFan[idxB] / GRAPH_MAX);
            for (int s = 0; s <= steps; ++s)
            {
                float t2 = (float)s / steps;
                float lx = xA + (xB - xA) * t2;
                float ly = yA + (yB - yA) * t2;
                if (lx >= GRAPH_X && lx <= GRAPH_R && ly >= GRAPH_Y && ly <= GRAPH_B)
                    FillRect(lx, ly - 1.f, lx + 2.f, ly + 1.f, COL_CYAN);
            }
        }
    }

    // Live dots at right edge
    if (s_histCount > 0 && s_sensorOK)
    {
        float dotX = GRAPH_R - 3.f;
        float cpuY = GRAPH_B - GRAPH_H * ((float)s_curCPU / GRAPH_MAX);
        float loadV = (s_state == SSTATE_RUNNING) ? 100.f : 0.f;
        float loadY = GRAPH_B - GRAPH_H * (loadV / GRAPH_MAX);
        FillRect(dotX - 3.f, cpuY - 3.f, dotX + 3.f, cpuY + 3.f, COL_YELLOW);
        FillRect(dotX - 3.f, loadY - 3.f, dotX + 3.f, loadY + 3.f,
            D3DCOLOR_XRGB(50, 255, 80));
        if (s_fanOK)
        {
            float fanY = GRAPH_B - GRAPH_H * ((float)((int)s_curFan * 2) / GRAPH_MAX);
            FillRect(dotX - 3.f, fanY - 3.f, dotX + 3.f, fanY + 3.f, COL_CYAN);
        }
    }

    // Legend (top-right inside graph)
    float lx = GRAPH_R - 130.f;
    float ly = GRAPH_Y + 5.f;
    FillRect(lx, ly, lx + 10.f, ly + 5.f, COL_YELLOW);
    DrawText(lx + 13.f, ly - 1.f, "CPU TEMP", 1.0f, COL_YELLOW);
    ly += 11.f;
    FillRect(lx, ly, lx + 10.f, ly + 5.f, D3DCOLOR_XRGB(50, 255, 80));
    DrawText(lx + 13.f, ly - 1.f, "LOAD %", 1.0f, D3DCOLOR_XRGB(50, 255, 80));
    ly += 11.f;
    FillRect(lx, ly, lx + 10.f, ly + 5.f, COL_CYAN);
    DrawText(lx + 13.f, ly - 1.f, "FAN %", 1.0f, COL_CYAN);

    // Time scale (only show seconds, not raw sample count)
    {
        char sc[32]; StrCopy(sc, sizeof(sc), "~");
        char t2[8];  IntToStr(s_histCount / 2, t2, sizeof(t2));
        StrCat2(sc, sizeof(sc), sc, t2);
        StrCat2(sc, sizeof(sc), sc, "s");
        DrawText(GRAPH_X + 4.f, GRAPH_Y + 4.f, sc, 1.0f, COL_DIM);
    }
}

// ============================================================================
// DrawConfirmOverlay — modal prompt, drawn on top of everything
// ============================================================================

static void DrawConfirmOverlay()
{
    float ox = SW * 0.5f - 160.f;
    float oy = SH * 0.5f - 50.f;
    float ow = 320.f;
    float oh = 100.f;

    FillRectGrad(ox, oy, ox + ow, oy + oh,
        D3DCOLOR_XRGB(20, 28, 70), D3DCOLOR_XRGB(12, 16, 46));
    HLine(oy, ox, ox + ow, COL_CYAN);
    HLine(oy + oh, ox, ox + ow, COL_CYAN);
    VLine(ox, oy, oy + oh, COL_CYAN);
    VLine(ox + ow, oy, oy + oh, COL_CYAN);

    const char* t1 = "START CPU STRESS TEST?";
    DrawText(ox + (ow - TW(t1, 1.3f)) * 0.5f, oy + 10.f, t1, 1.3f, COL_WHITE);

    const char* t2 = "Hold  LT + RT  simultaneously";
    DrawText(ox + (ow - TW(t2, 1.2f)) * 0.5f, oy + 34.f, t2, 1.2f, COL_YELLOW);

    const char* t3 = "to begin the test";
    DrawText(ox + (ow - TW(t3, 1.2f)) * 0.5f, oy + 52.f, t3, 1.2f, COL_YELLOW);

    const char* t4 = "[B]  Cancel";
    DrawText(ox + (ow - TW(t4, 1.1f)) * 0.5f, oy + 76.f, t4, 1.1f, COL_GRAY);
}

// ============================================================================
// DrawAbortBar — hold-progress shown when Back+A held during RUNNING
// ============================================================================

static void DrawAbortBar(DWORD holdMs)
{
    float frac = (float)holdMs / (float)ABORT_HOLD_MS;
    if (frac > 1.f) frac = 1.f;

    float bx = LM;
    float bw = SW - LM * 2.f;
    float by = ABORT_BAR_Y;

    FillRect(bx, by, bx + bw, by + ABORT_BAR_H, D3DCOLOR_XRGB(30, 10, 10));

    if (frac > 0.f)
    {
        FillRectGrad(bx, by, bx + bw * frac, by + ABORT_BAR_H,
            D3DCOLOR_XRGB(255, 60, 30), D3DCOLOR_XRGB(200, 20, 10));
    }

    HLine(by, bx, bx + bw, COL_BORDER);
    HLine(by + ABORT_BAR_H, bx, bx + bw, COL_BORDER);

    DWORD remain = (holdMs < ABORT_HOLD_MS)
        ? (ABORT_HOLD_MS - holdMs) / 1000 + 1
        : 0;
    char remBuf[40];
    StrCopy(remBuf, sizeof(remBuf), "ABORTING IN ");
    char remSec[8]; IntToStr((int)remain, remSec, sizeof(remSec));
    StrCat2(remBuf, sizeof(remBuf), remBuf, remSec);
    StrCat2(remBuf, sizeof(remBuf), remBuf, "s  --  RELEASE TO CANCEL");
    // Centre the label above the bar
    float lblX = LM + (LOAD_BAR_W - TW(remBuf, 1.05f)) * 0.5f;
    DrawText(lblX, by - 13.f, remBuf, 1.05f, COL_RED);
}

// ============================================================================
// DrawTabStrip
// ============================================================================

static void DrawTabStrip(StressCard active)
{
    bool cpuSel = (active == CARD_CPU);
    bool ramSel = (active == CARD_RAM);

    // CPU tab
    FillRectGrad(TAB_CPU_X, TAB_Y, TAB_CPU_X + TAB_W, TAB_Y + TAB_H,
        cpuSel ? D3DCOLOR_XRGB(40, 80, 160) : D3DCOLOR_XRGB(18, 25, 55),
        cpuSel ? D3DCOLOR_XRGB(20, 50, 110) : D3DCOLOR_XRGB(12, 16, 38));
    HLine(TAB_Y, TAB_CPU_X, TAB_CPU_X + TAB_W,
        cpuSel ? COL_CYAN : COL_BORDER);
    DrawText(TAB_CPU_X + 8.f, TAB_Y + 3.f, "CPU", 1.15f,
        cpuSel ? COL_WHITE : COL_DIM);

    // RAM tab
    FillRectGrad(TAB_RAM_X, TAB_Y, TAB_RAM_X + TAB_W, TAB_Y + TAB_H,
        ramSel ? D3DCOLOR_XRGB(40, 80, 160) : D3DCOLOR_XRGB(18, 25, 55),
        ramSel ? D3DCOLOR_XRGB(20, 50, 110) : D3DCOLOR_XRGB(12, 16, 38));
    HLine(TAB_Y, TAB_RAM_X, TAB_RAM_X + TAB_W,
        ramSel ? COL_CYAN : COL_BORDER);
    DrawText(TAB_RAM_X + 8.f, TAB_Y + 3.f, "RAM", 1.15f,
        ramSel ? COL_WHITE : COL_DIM);

    // Bottom rule under both tabs
    HLine(TAB_Y + TAB_H, LM, SW - LM, COL_BORDER);
}

// ============================================================================
// RenderCPUCard
// ============================================================================

static void RenderCPUCard(const DiagLogo& logo)
{
    const char* hint =
        (s_state == SSTATE_IDLE) ? "[A] Start Test    [B] Back    [Right] RAM Card" :
        (s_state == SSTATE_CONFIRM) ? "[LT+RT] Confirm    [B] Cancel" :
        "[Right] RAM Card    Hold [Back+A] 5s to Abort";

    DrawPageChrome(logo, "STRESS TEST  -  CPU", hint);
    DrawTabStrip(CARD_CPU);

    // Running badge + elapsed (top-right, inside content area)
    if (s_state == SSTATE_RUNNING)
    {
        DWORD secs = (GetTickCount() - s_testStartMs) / 1000;
        char elBuf[32];
        StrCopy(elBuf, sizeof(elBuf), "ELAPSED ");
        char secStr[12]; IntToStr((int)secs, secStr, sizeof(secStr));
        StrCat2(elBuf, sizeof(elBuf), elBuf, secStr);
        StrCat2(elBuf, sizeof(elBuf), elBuf, "s");

        DrawTextR(SW - LM, PANEL_TOP + 2.f, "* RUNNING *", 1.25f, COL_RED);
        DrawTextR(SW - LM, PANEL_TOP + 16.f, elBuf, 1.1f, COL_GRAY);
    }

    // ---- Info panels ----
    DrawInfoPanel(PANEL_T_X, PANEL_TOP, "CPU TEMP",
        (int)s_curCPU, CPU_WARN, CPU_HOT, "C", s_sensorOK);

    DrawInfoPanel(PANEL_F_X, PANEL_TOP, "FAN SPEED",
        s_fanOK ? (int)s_curFan * 2 : 0,
        0, 100, "%", s_fanOK);

    // MHz stat box — no gauge, just a large readout
    {
        float px = PANEL_M_X;
        float py = PANEL_TOP;
        float pw = PANEL_W;
        FillRectGrad(px, py, px + pw, py + PANEL_H,
            D3DCOLOR_XRGB(16, 20, 50), D3DCOLOR_XRGB(10, 13, 34));
        HLine(py, px, px + pw, COL_CYAN);
        HLine(py + PANEL_H, px, px + pw, COL_BORDER);
        VLine(px, py, py + PANEL_H, COL_BORDER);
        VLine(px + pw, py, py + PANEL_H, COL_BORDER);

        DrawText(px + P_PAD_X, py + P_LBL_DY, "CPU SPEED", 1.1f, COL_YELLOW);

        char mhzBuf[12]; IntToStr(s_curMHz, mhzBuf, sizeof(mhzBuf));
        char mhzDisp[20]; StrCat2(mhzDisp, sizeof(mhzDisp), mhzBuf, " MHz");
        DrawText(px + P_PAD_X, py + P_BIG_DY, mhzDisp, P_BIG_SC, COL_WHITE);

        const char* sLabel = (s_path == PATH_ADM1032) ? "ICS + ADM1032"
            : (s_path == PATH_PIC_16) ? "ICS + PIC"
            : "detecting...";
        DrawText(px + P_PAD_X, py + PANEL_H - 14.f, sLabel, 1.0f, COL_DIM);
    }

    // ---- CPU Load bar ----
    {
        int loadPct = (s_state == SSTATE_RUNNING) ? 100 : 0;

        DrawText(LOAD_BAR_X, LOAD_LBL_Y, "CPU LOAD", 1.1f, COL_YELLOW);

        float bx = LOAD_BAR_X;
        float by = LOAD_BAR_Y;
        float bw = LOAD_BAR_W;

        FillRect(bx, by, bx + bw, by + LOAD_BAR_H, D3DCOLOR_XRGB(14, 17, 38));

        if (loadPct > 0)
        {
            FillRectGrad(bx, by, bx + bw, by + LOAD_BAR_H,
                D3DCOLOR_XRGB(50, 220, 80), D3DCOLOR_XRGB(20, 140, 40));
        }

        char pctBuf[8]; IntToStr(loadPct, pctBuf, sizeof(pctBuf));
        char pctDisp[12]; StrCat2(pctDisp, sizeof(pctDisp), pctBuf, "%");
        float lblX = bx + (bw - TW(pctDisp, 1.15f)) * 0.5f;
        DrawText(lblX, by + 1.f, pctDisp, 1.15f, COL_WHITE);

        HLine(by, bx, bx + bw, COL_BORDER);
        HLine(by + LOAD_BAR_H, bx, bx + bw, COL_BORDER);
        VLine(bx, by, by + LOAD_BAR_H, COL_BORDER);
        VLine(bx + bw, by, by + LOAD_BAR_H, COL_BORDER);
    }

    // ---- Graph ----
    HLine(GRAPH_HDR_Y, LM, SW - LM, COL_BORDER);
    DrawText(LM, GRAPH_HDR_Y + 2.f, "LIVE TELEMETRY", 1.1f, COL_YELLOW);
    DrawGraph();

    // ---- Abort UI (only when running) ----
    if (s_state == SSTATE_RUNNING)
    {
        if (s_abortHolding)
            DrawAbortBar(GetTickCount() - s_abortHoldStart);
        else
            DrawText(LM, ABORT_BAR_Y - 2.f,
                "Hold  [Back + A]  for 5s to abort",
                1.0f, COL_DIM);
    }

    // ---- Confirm overlay drawn last (on top of everything) ----
    if (s_state == SSTATE_CONFIRM)
        DrawConfirmOverlay();
}

// ============================================================================

// ============================================================================
// RAM stress engine state
// All file-scope — mirrors RamTest's stress state machine exactly.
// ============================================================================

#define RAM_CHUNK_SIZE          (2  * 1024 * 1024)
#define RAM_BANK_SIZE_STD       (16 * 1024 * 1024)
#define RAM_BANK_SIZE_EXT       (32 * 1024 * 1024)
#define RAM_NUM_BANKS           4
#define RAM_CHUNKS_PER_BANK_STD 8
#define RAM_CHUNKS_PER_BANK_EXT 16
#define RAM_MAX_CHUNKS          16
#define RAM_PATTERN_A           0xAA55AA55UL
#define RAM_PATTERN_B           0x55AA55AAUL
#define RAM_TICK_MS             150   // time slice per tick

enum RamChunkState { RCHUNK_UNTESTED = 0, RCHUNK_SKIPPED, RCHUNK_TESTING, RCHUNK_PASS, RCHUNK_FAIL };

enum RamPhase
{
    RPHASE_ALLOC = 0,
    RPHASE_P1_WRITE,
    RPHASE_P2_READW,
    RPHASE_P3_READW,
    RPHASE_P4_READ,
    RPHASE_P5_WRITE,
    RPHASE_P6_READ,
    RPHASE_FREE,
};

struct RamChunk { RamChunkState state; DWORD errorCount; };
static RamChunk    sr_chunks[RAM_NUM_BANKS][RAM_MAX_CHUNKS];
static int         sr_chunksPerBank = RAM_CHUNKS_PER_BANK_STD;
static bool        sr_is128MB = false;

// Counters
static DWORD       sr_totalPhysMB = 0;
static DWORD       sr_availPhysMB = 0;
static DWORD       sr_usedMB = 0;
static int         sr_testedCount = 0;
static int         sr_passCount = 0;
static int         sr_failCount = 0;
static int         sr_skipCount = 0;
static DWORD       sr_totalErrors = 0;

// Sweep / timing
static int         sr_sweep = 0;
static DWORD       sr_sweepErrors = 0;
static DWORD       sr_startTick = 0;

// Per-bank stress state
static int         sr_bank = 0;
static RamPhase    sr_phase = RPHASE_ALLOC;
static DWORD* sr_base = NULL;
static DWORD       sr_dwords = 0;
static DWORD       sr_offset = 0;
static DWORD       sr_bankErr = 0;

// RAM card state machine — independent of CPU s_state
static StressState s_ramState = SSTATE_IDLE;
static bool        s_ramAbortHold = false;
static DWORD       s_ramAbortStart = 0;

extern "C" PVOID __stdcall MmAllocateContiguousMemory(ULONG NumberOfBytes);
extern "C" VOID  __stdcall MmFreeContiguousMemory(PVOID BaseAddress);

static __forceinline void RamFlushCache()
{
    __asm { wbinvd }
}

// ============================================================================
// RAM engine helpers
// ============================================================================

static void RamResetCounters()
{
    for (int b = 0; b < RAM_NUM_BANKS; ++b)
        for (int c = 0; c < RAM_MAX_CHUNKS; ++c)
        {
            sr_chunks[b][c].state = RCHUNK_UNTESTED; sr_chunks[b][c].errorCount = 0;
        }
    sr_testedCount = 0;
    sr_passCount = 0;
    sr_failCount = 0;
    sr_skipCount = 0;
    sr_totalErrors = 0;
}

static void RamOnStart()
{
    MEMORYSTATUS ms; ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    sr_totalPhysMB = (DWORD)(ms.dwTotalPhys / (1024 * 1024));
    sr_availPhysMB = (DWORD)(ms.dwAvailPhys / (1024 * 1024));
    sr_usedMB = sr_totalPhysMB - sr_availPhysMB;
    sr_is128MB = (sr_totalPhysMB >= 100);
    sr_chunksPerBank = sr_is128MB ? RAM_CHUNKS_PER_BANK_EXT : RAM_CHUNKS_PER_BANK_STD;

    if (sr_base) { MmFreeContiguousMemory(sr_base); sr_base = NULL; }
    RamResetCounters();
    sr_bank = 0;
    sr_phase = RPHASE_ALLOC;
    sr_offset = 0;
    sr_bankErr = 0;
    sr_sweep = 0;
    sr_sweepErrors = 0;
    sr_startTick = GetTickCount();
}

static void RamStop()
{
    if (sr_base) { MmFreeContiguousMemory(sr_base); sr_base = NULL; }
}

static const char* RamPhaseLabel(RamPhase ph)
{
    switch (ph)
    {
    case RPHASE_ALLOC:    return "ALLOCATING";
    case RPHASE_P1_WRITE: return "1/6  WRITE    fwd  0xAA55AA55";
    case RPHASE_P2_READW: return "2/6  READ+WRITE  fwd verify / inv write";
    case RPHASE_P3_READW: return "3/6  READ+WRITE  bwd verify / fwd write";
    case RPHASE_P4_READ:  return "4/6  READ     fwd verify 0xAA55AA55";
    case RPHASE_P5_WRITE: return "5/6  WRITE    addr XOR 0xDEADBEEF";
    case RPHASE_P6_READ:  return "6/6  READ     verify addr XOR";
    case RPHASE_FREE:     return "COMMITTING";
    default:              return "";
    }
}

static DWORD RamPhaseColor(RamPhase ph)
{
    switch (ph)
    {
    case RPHASE_P1_WRITE:
    case RPHASE_P5_WRITE: return D3DCOLOR_XRGB(220, 120, 20);
    case RPHASE_P2_READW:
    case RPHASE_P3_READW: return D3DCOLOR_XRGB(20, 180, 220);
    case RPHASE_P4_READ:
    case RPHASE_P6_READ:  return D3DCOLOR_XRGB(40, 200, 80);
    default:              return D3DCOLOR_XRGB(60, 60, 60);
    }
}

static float RamPhaseProgress()
{
    if (sr_dwords == 0) return 0.f;
    if (sr_phase == RPHASE_P3_READW && sr_offset < sr_dwords)
    {
        DWORD done = sr_dwords - 1 - sr_offset;
        return (float)done / (float)sr_dwords;
    }
    return (float)sr_offset / (float)sr_dwords;
}

static DWORD RamChunkColor(RamChunkState st, bool flash)
{
    switch (st)
    {
    case RCHUNK_PASS:    return COL_GREEN;
    case RCHUNK_FAIL:    return COL_RED;
    case RCHUNK_SKIPPED: return COL_GRAY;
    case RCHUNK_TESTING: return flash ? COL_YELLOW : D3DCOLOR_XRGB(200, 180, 0);
    default:             return COL_DIM;
    }
}

// ============================================================================
// RamStressStep  — time-sliced, call once per tick
// ============================================================================

static void RamStressStep()
{
    DWORD chunkDw = RAM_CHUNK_SIZE / sizeof(DWORD);

    // ---- ALLOC -------------------------------------------------------------
    if (sr_phase == RPHASE_ALLOC)
    {
        for (int c = 0; c < sr_chunksPerBank; ++c)
            sr_chunks[sr_bank][c].state = RCHUNK_TESTING;

        ULONG bankBytes = (ULONG)(sr_is128MB ? RAM_BANK_SIZE_EXT : RAM_BANK_SIZE_STD);
        ULONG tryBytes = bankBytes;
        sr_base = NULL;
        while (!sr_base && tryBytes >= (ULONG)RAM_CHUNK_SIZE)
        {
            sr_base = (DWORD*)MmAllocateContiguousMemory(tryBytes);
            if (!sr_base) tryBytes /= 2;
        }
        if (!sr_base)
        {
            for (int c = 0; c < sr_chunksPerBank; ++c)
            {
                sr_chunks[sr_bank][c].state = RCHUNK_SKIPPED; sr_skipCount++;
            }
            sr_phase = RPHASE_FREE;
            return;
        }
        sr_dwords = tryBytes / sizeof(DWORD);
        sr_offset = 0;
        sr_bankErr = 0;
        sr_phase = RPHASE_P1_WRITE;
        return;
    }

    // ---- FREE / commit -----------------------------------------------------
    if (sr_phase == RPHASE_FREE)
    {
        if (sr_base) { MmFreeContiguousMemory(sr_base); sr_base = NULL; }

        for (int c = 0; c < sr_chunksPerBank; ++c)
        {
            if (sr_chunks[sr_bank][c].state == RCHUNK_TESTING)
            {
                bool anyErr = (sr_chunks[sr_bank][c].errorCount > 0);
                sr_chunks[sr_bank][c].state = anyErr ? RCHUNK_FAIL : RCHUNK_PASS;
                sr_testedCount++;
                if (anyErr) sr_failCount++;
                else        sr_passCount++;
            }
        }
        sr_totalErrors += sr_bankErr;
        sr_bank++;

        if (sr_bank >= RAM_NUM_BANKS)
        {
            // Completed one sweep — reset grid and go again
            sr_sweep++;
            sr_sweepErrors += sr_totalErrors;
            RamResetCounters();
            sr_bank = 0;
        }
        sr_phase = RPHASE_ALLOC;
        sr_offset = 0;
        return;
    }

    // ---- Active pass — time-sliced -----------------------------------------
    DWORD tickStart = GetTickCount();
    DWORD* p = sr_base;
    DWORD  tot = sr_dwords;
    DWORD  off = sr_offset;

    if (sr_phase == RPHASE_P1_WRITE)
    {
        while (off < tot)
        {
            p[off] = RAM_PATTERN_A; off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off >= tot) { RamFlushCache(); sr_phase = RPHASE_P2_READW; sr_offset = 0; return; }
    }
    else if (sr_phase == RPHASE_P2_READW)
    {
        while (off < tot)
        {
            if (p[off] != RAM_PATTERN_A)
            {
                int c = (int)(off / chunkDw); if (c >= sr_chunksPerBank) c = sr_chunksPerBank - 1;
                sr_chunks[sr_bank][c].errorCount++; sr_bankErr++;
            }
            p[off] = RAM_PATTERN_B; off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off >= tot) { RamFlushCache(); sr_phase = RPHASE_P3_READW; sr_offset = tot - 1; return; }
    }
    else if (sr_phase == RPHASE_P3_READW)
    {
        while (off < tot)
        {
            if (p[off] != RAM_PATTERN_B)
            {
                int c = (int)(off / chunkDw); if (c >= sr_chunksPerBank) c = sr_chunksPerBank - 1;
                sr_chunks[sr_bank][c].errorCount++; sr_bankErr++;
            }
            p[off] = RAM_PATTERN_A;
            if (off == 0) { off = 0xFFFFFFFFUL; break; }
            off--;
            if ((off & 0x3FF) == 0x3FF && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off == 0xFFFFFFFFUL) { RamFlushCache(); sr_phase = RPHASE_P4_READ; sr_offset = 0; return; }
    }
    else if (sr_phase == RPHASE_P4_READ)
    {
        while (off < tot)
        {
            if (p[off] != RAM_PATTERN_A)
            {
                int c = (int)(off / chunkDw); if (c >= sr_chunksPerBank) c = sr_chunksPerBank - 1;
                sr_chunks[sr_bank][c].errorCount++; sr_bankErr++;
            }
            off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off >= tot) { sr_phase = RPHASE_P5_WRITE; sr_offset = 0; return; }
    }
    else if (sr_phase == RPHASE_P5_WRITE)
    {
        while (off < tot)
        {
            p[off] = off ^ 0xDEADBEEFUL; off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off >= tot) { RamFlushCache(); sr_phase = RPHASE_P6_READ; sr_offset = 0; return; }
    }
    else if (sr_phase == RPHASE_P6_READ)
    {
        while (off < tot)
        {
            if (p[off] != (off ^ 0xDEADBEEFUL))
            {
                int c = (int)(off / chunkDw); if (c >= sr_chunksPerBank) c = sr_chunksPerBank - 1;
                sr_chunks[sr_bank][c].errorCount++; sr_bankErr++;
            }
            off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off >= tot) { sr_phase = RPHASE_FREE; sr_offset = 0; return; }
    }

    sr_offset = off;
}

// ============================================================================
// RenderRAMCard
// ============================================================================
//
// Layout:
//   Tab strip                                     Y=TAB_Y..TAB_Y+TAB_H
//   COL_SPLIT = 300 — vertical divider
//   Left (x=LM..COL_SPLIT):
//     Memory map block (config/total/avail/used)
//     Bank address table (B0-B3)
//     Status rows: phase label + phase bar + sweep/errors/elapsed
//   Right (x=GRID_LM..608):
//     4-row chunk grid (identical to RamTest)
//     Lower: IDLE prompt / CONFIRM overlay / RUNNING abort bar
//
// The CONFIRM modal and RUNNING abort-hold bar live inside the card render
// so they overlay the grid naturally, same as the CPU card's overlays.
// ============================================================================

static void RenderRAMCard(const DiagLogo& logo)
{
    const char* hints;
    if (s_ramState == SSTATE_IDLE)
        hints = "[Left] CPU    [A] Start Stress    [B] Back";
    else if (s_ramState == SSTATE_CONFIRM)
        hints = "Hold LT+RT to confirm    [B] Cancel";
    else
        hints = "[Left] CPU    Hold Back+A 5s to Abort";

    DrawPageChrome(logo, "STRESS TEST  -  RAM", hints);
    DrawTabStrip(CARD_RAM);

    // ---- Shared layout constants (mirror RamTest's Render exactly) ---------
    const float COL_SPLIT = 300.f;
    const float MAP_LM = LM;
    const float GRID_LM = COL_SPLIT + 30.f;
    const float TS = 1.3f;
    const float LH = LINE_H - 2.f;
    const float SLH = LINE_H - 4.f;
    const float VM_ = MAP_LM + 90.f;    // value column for left panel

    float y = PANEL_TOP;    // start just below tab strip

    // ---- Section headers ---------------------------------------------------
    DrawText(MAP_LM, y, "MEMORY MAP", TS, COL_YELLOW);
    DrawText(GRID_LM, y, "PHYSICAL BANK MAP", TS, COL_YELLOW);
    y += LH + 2.f;
    HLine(y, MAP_LM, COL_SPLIT - 8.f, COL_BORDER);
    HLine(y, GRID_LM, SW - LM, COL_BORDER);
    y += 5.f;

    // ---- Left: RAM summary -------------------------------------------------
    char buf[48];

    DrawText(MAP_LM, y, "CONFIG  :", TS, COL_GRAY);
    DrawText(VM_, y, sr_is128MB ? "128MB" : "64MB", TS, COL_CYAN);
    y += LH;
    DrawText(VM_, y, sr_is128MB ? "4x32MB  dual rank" : "4x16MB  single rank", TS, COL_CYAN);
    y += LH;

    IntToStr((int)sr_totalPhysMB, buf, sizeof(buf));
    StrCat2(buf, sizeof(buf), buf, "MB");
    DrawText(MAP_LM, y, "TOTAL   :", TS, COL_GRAY);
    DrawText(VM_, y, buf, TS, COL_WHITE);
    y += LH;

    IntToStr((int)sr_availPhysMB, buf, sizeof(buf));
    StrCat2(buf, sizeof(buf), buf, "MB");
    DrawText(MAP_LM, y, "AVAIL   :", TS, COL_GRAY);
    DrawText(VM_, y, buf, TS, COL_WHITE);
    y += LH;

    IntToStr((int)sr_usedMB, buf, sizeof(buf));
    StrCat2(buf, sizeof(buf), buf, "MB");
    DrawText(MAP_LM, y, "IN USE  :", TS, COL_GRAY);
    DrawText(VM_, y, buf, TS, COL_ORANGE);
    y += LH + 4.f;

    // Bank address table
    HLine(y, MAP_LM, COL_SPLIT - 8.f, COL_BORDER);
    y += 5.f;
    DrawText(MAP_LM, y, "BANK", 1.15f, COL_DIM);
    DrawText(MAP_LM + 36.f, y, "BASE ADDR", 1.15f, COL_DIM);
    DrawText(MAP_LM + 122.f, y, "SIZE", 1.15f, COL_DIM);
    DrawText(MAP_LM + 162.f, y, "CHIPS", 1.15f, COL_DIM);
    y += LH;

    DWORD bankSizeMB = sr_is128MB ? 32 : 16;
    for (int b = 0; b < RAM_NUM_BANKS; ++b)
    {
        bool active = (s_ramState == SSTATE_RUNNING && sr_bank == b);
        DWORD rowCol = active ? COL_YELLOW : COL_WHITE;

        char bankBuf[4];
        IntToStr(b, bankBuf, sizeof(bankBuf));
        DrawText(MAP_LM, y, bankBuf, TS, active ? COL_CYAN : COL_GRAY);

        char addrHex[12], addrFull[14];
        IntToHex((DWORD)(b * (sr_is128MB ? RAM_BANK_SIZE_EXT : RAM_BANK_SIZE_STD)), 8, addrHex, sizeof(addrHex));
        StrCat2(addrFull, sizeof(addrFull), "0x", addrHex);
        DrawText(MAP_LM + 36.f, y, addrFull, TS, COL_CYAN);

        char szBuf[8];
        IntToStr((int)bankSizeMB, szBuf, sizeof(szBuf));
        StrCat2(szBuf, sizeof(szBuf), szBuf, "MB");
        DrawText(MAP_LM + 122.f, y, szBuf, TS, rowCol);
        DrawText(MAP_LM + 162.f, y, sr_is128MB ? "2" : "1", TS, rowCol);
        y += LH;
    }

    y += 4.f;
    HLine(y, MAP_LM, COL_SPLIT - 8.f, COL_BORDER);
    y += 5.f;

    // ---- Left lower: status block ------------------------------------------
    if (s_ramState == SSTATE_IDLE)
    {
        DrawText(MAP_LM, y, "[A] Start RAM Stress", TS, COL_YELLOW);
    }
    else
    {
        // Overall bank progress bar
        if (s_ramState == SSTATE_RUNNING)
        {
            int totalChunks = RAM_NUM_BANKS * sr_chunksPerBank;
            int doneChunks = sr_bank * sr_chunksPerBank;
            const float BAR_W = COL_SPLIT - MAP_LM - 8.f;
            const float BAR_H = 8.f;
            float fillW = (totalChunks > 0) ? BAR_W * ((float)doneChunks / (float)totalChunks) : 0.f;
            FillRect(MAP_LM, y, MAP_LM + BAR_W, y + BAR_H, D3DCOLOR_XRGB(20, 25, 55));
            if (fillW > 0.f)
                FillRectGrad(MAP_LM, y, MAP_LM + fillW, y + BAR_H,
                    D3DCOLOR_XRGB(60, 160, 255), D3DCOLOR_XRGB(30, 90, 180));
            HLine(y, MAP_LM, MAP_LM + BAR_W, COL_BORDER);
            HLine(y + BAR_H, MAP_LM, MAP_LM + BAR_W, COL_BORDER);
            VLine(MAP_LM, y, y + BAR_H, COL_BORDER);
            VLine(MAP_LM + BAR_W, y, y + BAR_H, COL_BORDER);

            char cb[4], tb[4];
            IntToStr(sr_bank + 1, cb, sizeof(cb));
            IntToStr(RAM_NUM_BANKS, tb, sizeof(tb));
            char progBuf[24];
            StrCat3(progBuf, sizeof(progBuf), "Bank ", cb, " of ");
            StrCat2(progBuf, sizeof(progBuf), progBuf, tb);
            DrawText(MAP_LM, y + BAR_H + 2.f, progBuf, TS, COL_YELLOW);
            y += BAR_H + SLH + 2.f;

            // Phase label
            DrawText(MAP_LM, y, "PHASE   :", TS, COL_GRAY);
            DrawText(MAP_LM + TW("PHASE   :", TS) + 4.f, y,
                RamPhaseLabel(sr_phase), 1.05f, COL_CYAN);
            y += SLH;

            // Phase progress bar
            float subProg = RamPhaseProgress();
            const float BAR_W2 = COL_SPLIT - MAP_LM - 8.f;
            const float BAR_H2 = 8.f;
            float fillW2 = BAR_W2 * subProg;
            DWORD barCol = RamPhaseColor(sr_phase);
            FillRect(MAP_LM, y, MAP_LM + BAR_W2, y + BAR_H2, D3DCOLOR_XRGB(15, 18, 40));
            float dw2 = fillW2 > 2.f ? fillW2 : 2.f;
            FillRect(MAP_LM, y, MAP_LM + dw2, y + BAR_H2, barCol);
            HLine(y, MAP_LM, MAP_LM + BAR_W2, COL_BORDER);
            HLine(y + BAR_H2, MAP_LM, MAP_LM + BAR_W2, COL_BORDER);
            VLine(MAP_LM, y, y + BAR_H2, COL_BORDER);
            VLine(MAP_LM + BAR_W2, y, y + BAR_H2, COL_BORDER);
            int pct = Ftoi(subProg * 100.f);
            char pctBuf[8];
            IntToStr(pct, pctBuf, sizeof(pctBuf));
            StrCat2(pctBuf, sizeof(pctBuf), pctBuf, "%");
            DrawText(MAP_LM + BAR_W2 - 28.f, y, pctBuf, 1.0f, D3DCOLOR_XRGB(220, 220, 220));
            y += BAR_H2 + 3.f;
        }

        // Elapsed / sweeps / error counts
        if (s_ramState == SSTATE_RUNNING || s_ramState == SSTATE_CONFIRM)
        {
            DWORD elSec = (GetTickCount() - sr_startTick) / 1000;
            char mm[4], ss[4];
            IntToStr((int)(elSec / 60), mm, sizeof(mm));
            IntToStr((int)(elSec % 60), ss, sizeof(ss));
            char elBuf[16];
            elBuf[0] = 0;
            StrCat2(elBuf, sizeof(elBuf), elBuf, mm);
            StrCat2(elBuf, sizeof(elBuf), elBuf, "m ");
            StrCat2(elBuf, sizeof(elBuf), elBuf, ss);
            StrCat2(elBuf, sizeof(elBuf), elBuf, "s");
            DrawText(MAP_LM, y, "ELAPSED :", TS, COL_GRAY);
            DrawText(VM_, y, elBuf, TS, COL_WHITE);
            y += SLH;

            char swBuf[8];
            IntToStr(sr_sweep, swBuf, sizeof(swBuf));
            DrawText(MAP_LM, y, "SWEEPS  :", TS, COL_GRAY);
            DrawText(VM_, y, swBuf, TS, COL_WHITE);
            y += SLH;

            DWORD totalErr = sr_sweepErrors + sr_totalErrors;
            UIntToStr(totalErr, buf, sizeof(buf));
            DrawText(MAP_LM, y, "ERRORS  :", TS, COL_GRAY);
            DrawText(VM_, y, buf, TS, totalErr > 0 ? COL_RED : COL_GREEN);
            y += SLH;

            IntToStr(sr_passCount, buf, sizeof(buf));
            DrawText(MAP_LM, y, "PASSED  :", TS, COL_GRAY);
            DrawText(VM_, y, buf, TS, COL_GREEN);
            y += SLH;

            IntToStr(sr_failCount, buf, sizeof(buf));
            DrawText(MAP_LM, y, "FAILED  :", TS, COL_GRAY);
            DrawText(VM_, y, buf, TS, sr_failCount > 0 ? COL_RED : COL_DIM);
            y += SLH;
        }
    }

    // ---- Vertical divider --------------------------------------------------
    VLine(COL_SPLIT, PANEL_TOP + LH + 7.f, BOT_BAR_Y - 4.f, COL_BORDER);

    // ---- Right: chunk grid (identical to RamTest's grid block) -------------
    {
        const float GRID_RIGHT = SW - 8.f;
        const float CELL_W = (GRID_RIGHT - GRID_LM) / (float)RAM_MAX_CHUNKS - 2.f;
        const float CELL_H = 38.f;
        const float CELL_PAD = 2.f;
        const float ROW_PAD = 8.f;
        bool flash = ((GetTickCount() / 200) & 1) != 0;
        float gridY = PANEL_TOP + LH + 7.f;

        for (int b = 0; b < RAM_NUM_BANKS; ++b)
        {
            float rowY = gridY + (float)b * (CELL_H + ROW_PAD);
            bool bankActive = (s_ramState == SSTATE_RUNNING && sr_bank == b);

            char bankLbl[4], bankRow[8];
            IntToStr(b, bankLbl, sizeof(bankLbl));
            StrCat2(bankRow, sizeof(bankRow), "B", bankLbl);
            DrawText(GRID_LM - 20.f, rowY + 10.f, bankRow, 1.2f,
                bankActive ? COL_YELLOW : COL_GRAY);

            for (int c = 0; c < sr_chunksPerBank; ++c)
            {
                float cx = GRID_LM + (float)c * (CELL_W + CELL_PAD);
                DWORD cellCol = RamChunkColor(sr_chunks[b][c].state, bankActive && flash);
                FillRect(cx, rowY, cx + CELL_W, rowY + CELL_H, cellCol);

                char mbBuf[8];
                if (sr_is128MB)
                    IntToStr(c, mbBuf, sizeof(mbBuf));
                else
                {
                    int mbStart = b * 16 + c * 2;
                    IntToStr(mbStart, mbBuf, sizeof(mbBuf));
                }
                DWORD lblCol = (sr_chunks[b][c].state == RCHUNK_PASS ||
                    sr_chunks[b][c].state == RCHUNK_SKIPPED)
                    ? COL_BG : D3DCOLOR_XRGB(40, 40, 40);
                DrawText(cx + 2.f, rowY + 4.f, mbBuf, 1.0f, lblCol);

                if (sr_chunks[b][c].state == RCHUNK_FAIL)
                {
                    char eBuf[8];
                    UIntToStr(sr_chunks[b][c].errorCount, eBuf, sizeof(eBuf));
                    DrawText(cx + 2.f, rowY + CELL_H - 12.f, eBuf, 1.0f,
                        D3DCOLOR_XRGB(255, 200, 200));
                }
            }

            // Dim unused columns for 64MB
            if (!sr_is128MB)
            {
                for (int c = RAM_CHUNKS_PER_BANK_STD; c < RAM_MAX_CHUNKS; ++c)
                {
                    float cx = GRID_LM + (float)c * (CELL_W + CELL_PAD);
                    FillRect(cx, rowY, cx + CELL_W, rowY + CELL_H, D3DCOLOR_XRGB(12, 14, 28));
                    HLine(rowY + CELL_H * 0.5f - 1.f, cx + 3.f, cx + CELL_W - 3.f, D3DCOLOR_XRGB(45, 45, 60));
                    VLine(cx + CELL_W * 0.5f, rowY + 3.f, rowY + CELL_H - 3.f, D3DCOLOR_XRGB(45, 45, 60));
                }
            }
        }

        // ---- Lower block: phase key + status / confirm / abort -------------
        float lowerY = gridY + (float)RAM_NUM_BANKS * (CELL_H + ROW_PAD) + 4.f;

        if (s_ramState == SSTATE_IDLE)
        {
            DrawText(GRID_LM, lowerY, "[A] Start    Runs continuously until you abort.", 1.15f, COL_YELLOW);
            lowerY += LINE_H;
            DrawText(GRID_LM, lowerY,
                sr_is128MB
                ? "128MB: chunks 0-7=CHIP1  8-15=CHIP2"
                : "64MB: any fail = that bank's chip suspect",
                1.1f, COL_CYAN);
        }
        else if (s_ramState == SSTATE_CONFIRM)
        {
            // Confirm overlay box
            const float OW = GRID_RIGHT - GRID_LM;
            const float OH = 56.f;
            FillRect(GRID_LM, lowerY, GRID_LM + OW, lowerY + OH, D3DCOLOR_ARGB(200, 10, 14, 40));
            HLine(lowerY, GRID_LM, GRID_LM + OW, COL_CYAN);
            HLine(lowerY + OH, GRID_LM, GRID_LM + OW, COL_CYAN);
            VLine(GRID_LM, lowerY, lowerY + OH, COL_CYAN);
            VLine(GRID_LM + OW, lowerY, lowerY + OH, COL_CYAN);

            DrawText(GRID_LM + 8.f, lowerY + 6.f,
                "RAM STRESS TEST — CONFIRM START", 1.25f, COL_YELLOW);
            DrawText(GRID_LM + 8.f, lowerY + 22.f,
                "Runs continuously. Hold LT + RT to begin.", 1.15f, COL_WHITE);
            DrawText(GRID_LM + 8.f, lowerY + 36.f,
                "[B] Cancel", 1.1f, COL_GRAY);
        }
        else // RUNNING
        {
            // Phase colour key
            HLine(lowerY, GRID_LM, GRID_RIGHT, COL_BORDER);
            lowerY += 4.f;

            FillRect(GRID_LM, lowerY + 3.f, GRID_LM + 10.f, lowerY + 11.f, D3DCOLOR_XRGB(220, 120, 20));
            DrawText(GRID_LM + 13.f, lowerY, "WRITE", 1.1f, COL_DIM);
            FillRect(GRID_LM + 65.f, lowerY + 3.f, GRID_LM + 75.f, lowerY + 11.f, D3DCOLOR_XRGB(20, 180, 220));
            DrawText(GRID_LM + 78.f, lowerY, "READ+WRITE", 1.1f, COL_DIM);
            FillRect(GRID_LM + 175.f, lowerY + 3.f, GRID_LM + 185.f, lowerY + 11.f, D3DCOLOR_XRGB(40, 200, 80));
            DrawText(GRID_LM + 188.f, lowerY, "READ/VERIFY", 1.1f, COL_DIM);
            lowerY += LINE_H;

            // Abort hold bar — same UX as CPU card
            if (s_ramAbortHold)
            {
                DWORD held = GetTickCount() - s_ramAbortStart;
                float abortFrac = held >= ABORT_HOLD_MS ? 1.f : (float)held / ABORT_HOLD_MS;
                const float ABW = GRID_RIGHT - GRID_LM;
                FillRect(GRID_LM, lowerY, GRID_LM + ABW, lowerY + 8.f, D3DCOLOR_XRGB(15, 10, 10));
                FillRect(GRID_LM, lowerY, GRID_LM + ABW * abortFrac, lowerY + 8.f, D3DCOLOR_XRGB(200, 30, 30));
                HLine(lowerY, GRID_LM, GRID_LM + ABW, COL_BORDER);
                HLine(lowerY + 8.f, GRID_LM, GRID_LM + ABW, COL_BORDER);
                lowerY += 10.f;

                int remSec = (int)((ABORT_HOLD_MS - (held < ABORT_HOLD_MS ? held : ABORT_HOLD_MS)) / 1000) + 1;
                char abortMsg[48];
                char secBuf[4];
                IntToStr(remSec, secBuf, sizeof(secBuf));
                abortMsg[0] = 0;
                StrCat2(abortMsg, sizeof(abortMsg), abortMsg, "ABORTING IN ");
                StrCat2(abortMsg, sizeof(abortMsg), abortMsg, secBuf);
                StrCat2(abortMsg, sizeof(abortMsg), abortMsg, "s  --  RELEASE TO CANCEL");
                DrawText(GRID_LM, lowerY, abortMsg, 1.15f, COL_RED);
            }
            else
            {
                DrawText(GRID_LM, lowerY, "Hold Back+A for 5s to abort.", 1.1f, COL_DIM);
            }
        }
    }
}


// ============================================================================
// HandleInput
// ============================================================================

static void HandleInput()
{
    WORD cur = GetButtons();
    int lt = 0, rt = 0, blk = 0, wht = 0, btnA = 0, btnB = 0, btnX = 0, btnY = 0;
    GetTriggers(lt, rt, blk, wht, btnA, btnB, btnX, btnY);
    (void)blk; (void)wht; (void)btnA; (void)btnB; (void)btnX; (void)btnY;

    // Card navigation — only when nothing is running on either card
    bool anythingRunning = (s_state == SSTATE_RUNNING ||
        s_ramState == SSTATE_RUNNING ||
        s_state == SSTATE_CONFIRM ||
        s_ramState == SSTATE_CONFIRM);
    if (!anythingRunning)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_RIGHT)) s_card = CARD_RAM;
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_LEFT))  s_card = CARD_CPU;
    }

    // Back to menu — only from full idle on both cards
    if (s_state == SSTATE_IDLE && s_ramState == SSTATE_IDLE)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_B) ||
            EdgeDown(cur, s_prevBtns, BTN_BACK))
        {
            s_prevBtns = cur;
            RequestState(MSTATE_MENU);
            return;
        }
    }

    // ---- CPU card input ----------------------------------------------------
    if (s_card == CARD_CPU)
    {
        switch (s_state)
        {
        case SSTATE_IDLE:
            if (EdgeDown(cur, s_prevBtns, BTN_A))
                s_state = SSTATE_CONFIRM;
            break;

        case SSTATE_CONFIRM:
            if (EdgeDown(cur, s_prevBtns, BTN_B))
            {
                s_state = SSTATE_IDLE; break;
            }
            if (lt > 128 && rt > 128)
            {
                s_state = SSTATE_RUNNING;
                s_testStartMs = GetTickCount();
                s_abortHolding = false;
                s_histHead = 0;
                s_histCount = 0;
            }
            break;

        case SSTATE_RUNNING:
        {
            bool backHeld = (cur & BTN_BACK) != 0;
            bool aHeld = (cur & BTN_A) != 0;
            if (backHeld && aHeld)
            {
                if (!s_abortHolding)
                {
                    s_abortHolding = true; s_abortHoldStart = GetTickCount();
                }
                else if ((GetTickCount() - s_abortHoldStart) >= ABORT_HOLD_MS)
                {
                    s_state = SSTATE_IDLE; s_abortHolding = false;
                }
            }
            else
                s_abortHolding = false;
        }
        break;
        }
    }

    // ---- RAM card input ----------------------------------------------------
    if (s_card == CARD_RAM)
    {
        switch (s_ramState)
        {
        case SSTATE_IDLE:
            if (EdgeDown(cur, s_prevBtns, BTN_A))
            {
                RamOnStart();
                s_ramState = SSTATE_CONFIRM;
            }
            break;

        case SSTATE_CONFIRM:
            if (EdgeDown(cur, s_prevBtns, BTN_B))
            {
                s_ramState = SSTATE_IDLE; break;
            }
            if (lt > 128 && rt > 128)
            {
                s_ramState = SSTATE_RUNNING;
                s_ramAbortHold = false;
            }
            break;

        case SSTATE_RUNNING:
        {
            bool backHeld = (cur & BTN_BACK) != 0;
            bool aHeld = (cur & BTN_A) != 0;
            if (backHeld && aHeld)
            {
                if (!s_ramAbortHold)
                {
                    s_ramAbortHold = true; s_ramAbortStart = GetTickCount();
                }
                else if ((GetTickCount() - s_ramAbortStart) >= ABORT_HOLD_MS)
                {
                    RamStop();
                    s_ramState = SSTATE_IDLE;
                    s_ramAbortHold = false;
                }
            }
            else
                s_ramAbortHold = false;
        }
        break;
        }
    }

    s_prevBtns = cur;
}

// ============================================================================
// StressTest_Tick
// ============================================================================

void StressTest_Tick(const DiagLogo& logo)
{
    HandleInput();

    // Sensor sample on interval
    DWORD now = GetTickCount();
    if ((now - s_lastSample) >= SAMPLE_INTERVAL_MS)
    {
        TakeSample();
        s_curMHz = ReadCPUMHz();
        s_lastSample = now;
    }

    // Render frame
    g_pDevice->BeginScene();
    if (s_card == CARD_CPU)
        RenderCPUCard(logo);
    else
        RenderRAMCard(logo);
    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);

    // CPU FPU stress — burns full inter-frame gap after Present
    if (s_card == CARD_CPU && s_state == SSTATE_RUNNING)
    {
        DWORD deadline = GetTickCount() + 30;
        FPUStress(deadline);
    }

    // RAM stress step — time-sliced, runs after Present on RAM card
    if (s_card == CARD_RAM && s_ramState == SSTATE_RUNNING)
    {
        RamStressStep();
    }
}