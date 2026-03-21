// StressTest.cpp
// XbDiag - Stress Test module.
//
// Card 0: CPU stress test (Prime95-style FPU torture)
// Card 1: RAM stress test (moving inversions soak, independent state machine)
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
// Coppermine Pentium III (733MHz):
//   L1: 32KB data (working set sized to fit exactly)
//   L2: 256KB on-die half-speed
//   FPU: fully pipelined, dual-issue FMUL+FADD on independent ops
//
// Kernels sourced from Prime95 gwnum library (lucas.mac).
// All four passes from the one-pass x87 DWT small-FFT torture path:
//
//   EightRealsSweep      eight_reals_fft_cmn    52 clocks/block   2 fmuls
//   FourComplexSweep     four_complex_fft_cmn   64 clocks/block  12 fmuls
//   FourComplexSquare    four_complex_square   144 clocks/block  32 fmuls
//   FourComplexUnfftSweep four_complex_unfft_cmn 65 clocks/block 12 fmuls
//
// Working set: SM_N=4096 doubles = 32KB, fills Coppermine L1 exactly.
// See StressMath.cpp for kernel implementation and coverage numbers.
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
#include "StressTestCPU.h"
#include "StressTestRAM.h"
#include "StressMath.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

// ============================================================================

static const DWORD SAMPLE_INTERVAL_MS = 500;   // idle/confirm/threshold
static const DWORD SAMPLE_INTERVAL_BURN_MS = 1500;  // running — reduces SMBus traffic during soak

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

StressState s_state = SSTATE_IDLE;
StressCard  s_card = CARD_CPU;
static WORD s_stPrevBtns = 0;

BYTE         s_curCPU = 0;
BYTE         s_curMB = 0;
BYTE         s_curFan = 0;
int          s_curMHz = 733;
bool         s_sensorOK = false;
bool         s_fanOK = false;
bool         s_mbOK = false;
bool         s_pathKnown = false;
bool         s_is16 = false;
BYTE  s_histCPU[HISTORY_LEN];
BYTE  s_histLoad[HISTORY_LEN];
BYTE  s_histFan[HISTORY_LEN];
int   s_histHead = 0;
int   s_histCount = 0;
DWORD        s_lastSample = 0;
DWORD        s_testStartMs = 0;
BYTE         s_minCPU = 255;
BYTE         s_maxCPU = 0;
BYTE         s_minFan = 255;
BYTE         s_maxFan = 0;
bool  s_abortHolding = false;
DWORD s_abortHoldStart = 0;
int          s_tempThreshold = 75;
bool         s_thermalAbort = false;
static const DWORD RENDER_INTERVAL_MS = 500;
DWORD        s_nextRender = 0;
DWORD        s_burnAccumMs = 0;
DWORD        s_windowStartMs = 0;
BYTE         s_measuredLoad = 0;

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

    if (s_state == SSTATE_RUNNING)
    {
        if (cpu < s_minCPU) s_minCPU = cpu;
        if (cpu > s_maxCPU) s_maxCPU = cpu;
        if (fan < s_minFan) s_minFan = fan;
        if (fan > s_maxFan) s_maxFan = fan;
    }
}

// ============================================================================
// OnEnter
// ============================================================================

void StressTest_OnEnter()
{
    s_stPrevBtns = GetButtons();
    s_state = SSTATE_IDLE;
    s_card = CARD_CPU;
    s_histHead = 0;
    s_histCount = 0;
    s_curCPU = 0;
    s_curMB = 0;
    s_curFan = 0;
    s_curMHz = 733;
    s_sensorOK = false;
    s_fanOK = false;
    s_mbOK = false;
    s_pathKnown = false;
    s_is16 = false;
    s_abortHolding = false;
    s_abortHoldStart = 0;
    s_thermalAbort = false;
    // Note: s_tempThreshold intentionally NOT reset — persists across visits
    s_lastSample = GetTickCount() - SAMPLE_INTERVAL_MS;

    // Seed working buffer and set SQRTHALF for the CPU stress kernel.
    StressMath_Init();
}

// ============================================================================
// RAM stress engine state
// All file-scope — mirrors RamTest's stress state machine exactly.
// ============================================================================

#define RAM_CHUNK_SIZE          (2  * 1024 * 1024)
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
        if (ST_EdgeDown(cur, s_stPrevBtns, BTN_DPAD_RIGHT)) s_card = CARD_RAM;
        if (ST_EdgeDown(cur, s_stPrevBtns, BTN_DPAD_LEFT))  s_card = CARD_CPU;
    }

    // Back to menu — only from full idle on both cards
    if (s_state == SSTATE_IDLE && s_ramState == SSTATE_IDLE)
    {
        if (ST_EdgeDown(cur, s_stPrevBtns, BTN_B) ||
            ST_EdgeDown(cur, s_stPrevBtns, BTN_BACK))
        {
            s_stPrevBtns = cur;
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
            if (ST_EdgeDown(cur, s_stPrevBtns, BTN_A))
            {
                s_thermalAbort = false;
                s_state = SSTATE_THRESHOLD;
            }
            break;

        case SSTATE_THRESHOLD:
            if (ST_EdgeDown(cur, s_stPrevBtns, BTN_B))
            {
                s_state = SSTATE_IDLE; break;
            }
            if (ST_EdgeDown(cur, s_stPrevBtns, BTN_LTRIG))
            {
                if (s_tempThreshold > 60) s_tempThreshold -= 5;
                break;
            }
            if (ST_EdgeDown(cur, s_stPrevBtns, BTN_RTRIG))
            {
                if (s_tempThreshold < 90) s_tempThreshold += 5;
                break;
            }
            if (ST_EdgeDown(cur, s_stPrevBtns, BTN_A))
            {
                s_state = SSTATE_CONFIRM;
            }
            break;

        case SSTATE_CONFIRM:
            if (ST_EdgeDown(cur, s_stPrevBtns, BTN_B))
            {
                s_state = SSTATE_IDLE; break;
            }
            if (lt > 128 && rt > 128)
            {
                s_state = SSTATE_RUNNING;
                s_testStartMs = GetTickCount();
                s_abortHolding = false;
                s_nextRender = 0;
                s_burnAccumMs = 0;
                s_windowStartMs = GetTickCount();
                s_measuredLoad = 0;
                s_histHead = 0;
                s_histCount = 0;
                s_minCPU = 255; s_maxCPU = 0;
                s_minFan = 255; s_maxFan = 0;
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
            if (ST_EdgeDown(cur, s_stPrevBtns, BTN_A))
            {
                ST_RAM_OnStart();
                s_ramState = SSTATE_CONFIRM;
            }
            break;

        case SSTATE_CONFIRM:
            if (ST_EdgeDown(cur, s_stPrevBtns, BTN_B))
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
                    ST_RAM_Stop();
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

    s_stPrevBtns = cur;
}

// ============================================================================
// StressTest_Tick
// ============================================================================

// ============================================================================
// StressTest_Tick
// ============================================================================

void StressTest_Tick(const DiagLogo& logo)
{
    HandleInput();

    if (s_card == CARD_CPU && s_state == SSTATE_RUNNING)
    {
        // ── Savage CPU burn loop ─────────────────────────────────────────────
        // Load measurement uses fixed, edge-aligned windows so the denominator
        // is always exactly SAMPLE_INTERVAL_BURN_MS regardless of when sensor
        // reads or renders happen to fire.
        //
        //   load = burnAccumMs / SAMPLE_INTERVAL_BURN_MS   (fixed divisor)
        //   s_windowStartMs += SAMPLE_INTERVAL_BURN_MS     (aligned advance)
        //
        // This eliminates the ±2-5% wobble that comes from using the actual
        // elapsed time as the denominator when that time drifts due to render
        // or SMBus overhead landing inside the window boundary.

        DWORD now = GetTickCount();

        // ── Window close + sensor sample ─────────────────────────────────────
        if ((now - s_windowStartMs) >= SAMPLE_INTERVAL_BURN_MS)
        {
            DWORD load = (s_burnAccumMs * 100) / SAMPLE_INTERVAL_BURN_MS;
            s_measuredLoad = (load > 100) ? 100 : (BYTE)load;
            s_burnAccumMs = 0;
            s_windowStartMs += SAMPLE_INTERVAL_BURN_MS;
            ST_CPU_TakeSample();
            ST_CPU_ReadMHz();
            s_lastSample = s_windowStartMs;
        }

        // ── Render at ~10fps ─────────────────────────────────────────────────
        if (now >= s_nextRender)
        {
            g_pDevice->BeginScene();
            ST_CPU_Render(logo);
            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);
            s_nextRender = GetTickCount() + RENDER_INTERVAL_MS;
        }

        // ── Burn until 2ms before the next window boundary ───────────────────
        // burnUntil is the end of the current (not yet closed) window.
        // CPUStress runs x87 + SSE kernels for the bulk of the window.
        // MemFlood_Timed follows as a tail to exercise FSB/DRAM.
        //
        // burnStart is captured here — after any window-close that may have
        // already fired above — so s_burnAccumMs never spans more than one
        // window and the load calculation can't exceed 100%.
        {
            DWORD burnStart = GetTickCount();
            DWORD burnUntil = s_windowStartMs + SAMPLE_INTERVAL_BURN_MS - 2;
            static const DWORD MEM_SLICE_MS = 300;  // 4 memory patterns need more time
            if (burnStart < burnUntil)
            {
                DWORD cpuDeadline = burnUntil - MEM_SLICE_MS;
                if (burnStart < cpuDeadline)
                    CPUStress(cpuDeadline);

                DWORD memStart = GetTickCount();
                if (memStart < burnUntil)
                    MemFlood_Timed(burnUntil);

                s_burnAccumMs += GetTickCount() - burnStart;
            }
        }
    }
    else
    {
        // ── Normal tick (idle/threshold/confirm, or RAM card) ────────────────
        DWORD now = GetTickCount();
        if ((now - s_lastSample) >= SAMPLE_INTERVAL_MS)
        {
            ST_CPU_TakeSample();
            ST_CPU_ReadMHz();
            s_lastSample = now;
        }

        g_pDevice->BeginScene();
        if (s_card == CARD_CPU)
            ST_CPU_Render(logo);
        else
            ST_RAM_Render(logo);
        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
    }

    // ── RAM stress step (time-sliced, runs every tick on RAM card) ───────────
    if (s_card == CARD_RAM && s_ramState == SSTATE_RUNNING)
    {
        ST_RAM_Step();
    }
}
// ============================================================================
// StressTest_AutoRun — headless CPU stress for XbSet automation
// Uses the exact same burn loop as StressTest_Tick (CARD_CPU / SSTATE_RUNNING):
//   same CPUStress kernel, same TakeSample, same edge-aligned measurement windows.
// Runs for durationMs milliseconds then writes results to hReport.
// ============================================================================

void StressTest_AutoRun(HANDLE hReport, DWORD durationMs)
{
    // Full init — seeds working buffer, resets all sensor state
    StressTest_OnEnter();

    // Wire up running state directly — bypass threshold/confirm UI
    s_state = SSTATE_RUNNING;
    s_testStartMs = GetTickCount();
    s_burnAccumMs = 0;
    s_windowStartMs = GetTickCount();
    s_measuredLoad = 0;
    s_minCPU = 255; s_maxCPU = 0;
    s_minFan = 255; s_maxFan = 0;
    s_thermalAbort = false;
    s_nextRender = 0;

    DWORD endTime = GetTickCount() + durationMs;

    while (GetTickCount() < endTime && s_state == SSTATE_RUNNING)
    {
        DWORD now = GetTickCount();

        // Window close + sensor sample (mirrors StressTest_Tick exactly)
        if ((now - s_windowStartMs) >= SAMPLE_INTERVAL_BURN_MS)
        {
            DWORD load = (s_burnAccumMs * 100) / SAMPLE_INTERVAL_BURN_MS;
            s_measuredLoad = (load > 100) ? 100 : (BYTE)load;
            s_burnAccumMs = 0;
            s_windowStartMs += SAMPLE_INTERVAL_BURN_MS;
            ST_CPU_TakeSample();
            ST_CPU_ReadMHz();
            s_lastSample = s_windowStartMs;
        }

        // Render status every ~500ms so screen doesn't appear frozen
        DWORD nowR = GetTickCount();
        if (nowR >= s_nextRender)
        {
            DWORD elapsed = nowR - (endTime - durationMs);
            DWORD remain = (nowR < endTime) ? (endTime - nowR) / 1000 : 0;
            if (g_pDevice)
            {
                g_pDevice->BeginScene();
                DWORD dim = D3DCOLOR_XRGB(10, 13, 30);
                g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET, dim, 1.f, 0);
                float py = 40.f;
                DrawText(12.f, py, "CPU STRESS TEST (AUTO)", 1.4f,
                    D3DCOLOR_XRGB(255, 220, 60)); py += 24.f;
                char sb[64]; char rm[8], cp[8], ld[8];
                IntToStr((int)remain, rm, sizeof(rm));
                IntToStr((int)s_maxCPU, cp, sizeof(cp));
                IntToStr((int)s_measuredLoad, ld, sizeof(ld));
                StrCopy(sb, sizeof(sb), "Remaining: ");
                StrCat2(sb, sizeof(sb), sb, rm);
                StrCat2(sb, sizeof(sb), sb, "s   Peak CPU: ");
                StrCat2(sb, sizeof(sb), sb, cp);
                StrCat2(sb, sizeof(sb), sb, "C   Load: ");
                StrCat2(sb, sizeof(sb), sb, ld);
                StrCat2(sb, sizeof(sb), sb, "%");
                DrawText(12.f, py, sb, 1.2f,
                    D3DCOLOR_XRGB(180, 180, 180));
                g_pDevice->EndScene();
                g_pDevice->Present(NULL, NULL, NULL, NULL);
            }
            s_nextRender = nowR + 500;
        }

        // Burn until 2ms before next window boundary
        {
            DWORD burnUntil = s_windowStartMs + SAMPLE_INTERVAL_BURN_MS - 2;
            static const DWORD MEM_SLICE_MS = 300;
            DWORD burnStart = GetTickCount();
            if (burnStart < burnUntil)
            {
                DWORD cpuDeadline = (burnUntil > MEM_SLICE_MS)
                    ? burnUntil - MEM_SLICE_MS : burnUntil;
                if (burnStart < cpuDeadline)
                    CPUStress(cpuDeadline);
                DWORD memStart = GetTickCount();
                if (memStart < burnUntil)
                    MemFlood_Timed(burnUntil);
                s_burnAccumMs += GetTickCount() - burnStart;
            }
        }
    }

    // Stop the burn
    s_state = SSTATE_IDLE;

    if (!hReport || hReport == INVALID_HANDLE_VALUE) return;

    // Write report
    char line[256]; DWORD w;
    auto WL = [&](const char* lbl, const char* val)
        {
            StrCopy(line, sizeof(line), lbl);
            StrCat2(line, sizeof(line), line, val);
            StrCat2(line, sizeof(line), line, "\r\n");
            WriteFile(hReport, line, StrLen(line), &w, NULL);
        };

    // Duration
    char t[32];
    DWORD totalSecs = durationMs / 1000;
    if (totalSecs >= 3600)
    {
        char hh[8], mm[8], ss[8];
        IntToStr((int)(totalSecs / 3600), hh, sizeof(hh));
        IntToStr((int)((totalSecs % 3600) / 60), mm, sizeof(mm));
        IntToStr((int)(totalSecs % 60), ss, sizeof(ss));
        StrCopy(t, sizeof(t), hh); StrCat2(t, sizeof(t), t, "h ");
        StrCat2(t, sizeof(t), t, mm); StrCat2(t, sizeof(t), t, "m ");
        StrCat2(t, sizeof(t), t, ss); StrCat2(t, sizeof(t), t, "s");
    }
    else
    {
        char mm[8], ss[8];
        IntToStr((int)(totalSecs / 60), mm, sizeof(mm));
        IntToStr((int)(totalSecs % 60), ss, sizeof(ss));
        StrCopy(t, sizeof(t), mm); StrCat2(t, sizeof(t), t, "m ");
        StrCat2(t, sizeof(t), t, ss); StrCat2(t, sizeof(t), t, "s");
    }
    WL("Duration:       ", t);

    // Temperature range
    char minT[8], maxT[8];
    IntToStr((int)s_minCPU, minT, sizeof(minT));
    IntToStr((int)s_maxCPU, maxT, sizeof(maxT));
    StrCopy(t, sizeof(t), minT); StrCat2(t, sizeof(t), t, " C  ->  ");
    StrCat2(t, sizeof(t), t, maxT); StrCat2(t, sizeof(t), t, " C");
    WL("CPU Temp range: ", t);

    // Fan speed range
    if (s_minFan != 255)
    {
        char minF[8], maxF[8];
        IntToStr((int)s_minFan * 2, minF, sizeof(minF));
        IntToStr((int)s_maxFan * 2, maxF, sizeof(maxF));
        StrCopy(t, sizeof(t), minF); StrCat2(t, sizeof(t), t, "%  ->  ");
        StrCat2(t, sizeof(t), t, maxF); StrCat2(t, sizeof(t), t, "%");
        WL("Fan Speed range:", t);
    }
    else
    {
        WL("Fan Speed:      ", "Not detected");
    }

    // Load
    IntToStr((int)s_measuredLoad, t, sizeof(t));
    StrCat2(t, sizeof(t), t, "%");  WL("Final Load:     ", t);

    // Thermal abort
    WL("Thermal Abort:  ", s_thermalAbort ? "YES - threshold hit" : "No");

    // Overall result
    if (s_thermalAbort)
        WL("Result:         ", "ABORTED - thermal protection triggered");
    else if (s_maxCPU >= 85)
        WL("Result:         ", "WARNING - peak temperature above 85C");
    else
        WL("Result:         ", "PASS");
}

// ============================================================================
// AutoRun result accessors
// ============================================================================

BYTE  StressAutoRun_GetMinCPU() { return s_minCPU; }
BYTE  StressAutoRun_GetMaxCPU() { return s_maxCPU; }
BYTE  StressAutoRun_GetMinFan() { return s_minFan; }
BYTE  StressAutoRun_GetMaxFan() { return s_maxFan; }
BYTE  StressAutoRun_GetMeasuredLoad() { return s_measuredLoad; }
bool  StressAutoRun_GetThermalAbort() { return s_thermalAbort; }

void ST_PushHistory(BYTE cpu, BYTE load, BYTE fan) { PushHistory(cpu, load, fan); }