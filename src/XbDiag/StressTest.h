#pragma once
// StressTest.h
// XbDiag - Stress Test shared types and state.
//
// StressState, StressCard, and the CPU sensor/nav state are defined in
// StressTest.cpp and exposed here so StressTestRAM can access them directly.

#include "DiagCommon.h"
#include <xtl.h>

// ============================================================================
// Shared types
// ============================================================================

enum StressState { SSTATE_IDLE = 0, SSTATE_THRESHOLD, SSTATE_CONFIRM, SSTATE_RUNNING };
enum StressCard { CARD_CPU = 0, CARD_RAM = 1 };

// ============================================================================
// Shared constants (used by both CPU and RAM render)
// ============================================================================

#define ABORT_HOLD_MS   5000

// Layout constant shared with StressTestRAM (RenderRAMCard)
// PANEL_TOP = TAB_Y + TAB_H + 1 = CONTENT_Y + 16 + 1
#define ST_PANEL_TOP    (CONTENT_Y + 17.f)


// ============================================================================
// CPU config constants (used by StressTestCPU render functions)
// ============================================================================

#define CPU_WARN   65
#define CPU_HOT    80
#define GRAPH_MAX  100
#define HISTORY_LEN 128

// ============================================================================
// CPU layout constants
// ============================================================================

static const float ST_TAB_Y = CONTENT_Y;
static const float ST_TAB_H = 16.f;
static const float ST_TAB_W = 56.f;
static const float ST_TAB_CPU_X = LM;
static const float ST_TAB_RAM_X = LM + 56.f + 4.f;

static const float ST_PANEL_H = 70.f;
static const float ST_PANEL_W = 182.f;
static const float ST_PANEL_GAP = 6.f;
static const float ST_PANEL_T_X = LM;
static const float ST_PANEL_F_X = LM + 182.f + 6.f;
static const float ST_PANEL_M_X = LM + (182.f + 6.f) * 2.f;

static const float ST_P_LBL_DY = 4.f;
static const float ST_P_BIG_DY = 16.f;
static const float ST_P_BAR_DY = 50.f;
static const float ST_P_BAR_H = 12.f;
static const float ST_P_BIG_SC = 2.8f;
static const float ST_P_PAD_X = 8.f;

static const float ST_LOAD_LBL_Y = ST_PANEL_TOP + ST_PANEL_H + 5.f;
static const float ST_LOAD_BAR_Y = ST_LOAD_LBL_Y + 13.f;
static const float ST_LOAD_BAR_H = 14.f;
static const float ST_LOAD_BAR_X = LM;
static const float ST_LOAD_BAR_W = SW - LM * 2.f;

static const float ST_GRAPH_HDR_Y = ST_LOAD_BAR_Y + ST_LOAD_BAR_H + 5.f;
static const float ST_GRAPH_X = LM + 28.f;
static const float ST_GRAPH_Y = ST_GRAPH_HDR_Y + 13.f;
static const float ST_GRAPH_W = SW - ST_GRAPH_X - LM - 2.f;
static const float ST_GRAPH_H = BOT_BAR_Y - ST_GRAPH_Y - 34.f;
static const float ST_GRAPH_R = ST_GRAPH_X + ST_GRAPH_W;
static const float ST_GRAPH_B = ST_GRAPH_Y + ST_GRAPH_H;

static const float ST_ABORT_BAR_Y = BOT_BAR_Y - 12.f;
static const float ST_ABORT_BAR_H = 6.f;

// ============================================================================
// Shared state — defined in StressTest.cpp
// ============================================================================

extern StressState s_state;
extern StressCard  s_card;

// CPU sensor (read by RenderRAMCard for nothing, but RAM needs s_state/s_card)
extern BYTE  s_curCPU;
extern BYTE  s_curFan;
extern bool  s_sensorOK;
extern bool  s_fanOK;
extern bool  s_mbOK;
extern BYTE  s_curMB;
extern bool  s_pathKnown;
extern bool  s_is16;
extern int   s_curMHz;
extern BYTE  s_minCPU;
extern BYTE  s_maxCPU;
extern BYTE  s_minFan;
extern BYTE  s_maxFan;
extern DWORD s_burnAccumMs;
extern DWORD s_windowStartMs;
extern DWORD s_nextRender;
extern DWORD s_lastSample;

// History ring buffers (read by DrawGraph in StressTestCPU)
extern BYTE  s_histCPU[HISTORY_LEN];
extern BYTE  s_histLoad[HISTORY_LEN];
extern BYTE  s_histFan[HISTORY_LEN];
extern int   s_histHead;
extern int   s_histCount;

// Abort hold state (read by DrawAbortBar in StressTestCPU)
extern bool  s_abortHolding;
extern DWORD s_abortHoldStart;
extern int   s_tempThreshold;
extern bool  s_thermalAbort;
extern BYTE  s_measuredLoad;
extern DWORD s_testStartMs;

// RAM card state — defined in StressTestRAM.cpp, used by HandleInput and Tick
extern StressState s_ramState;
extern bool        s_ramAbortHold;
extern DWORD       s_ramAbortStart;

// ============================================================================
// Shared inline helpers
// ============================================================================

static inline bool ST_EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

static inline DWORD ST_TempColor(int v, int warn, int hot)
{
    if (v >= hot)  return COL_RED;
    if (v >= warn) return COL_ORANGE;
    return COL_GREEN;
}

// PushHistory — defined in StressTest.cpp, called by TakeSample in StressTestCPU
void ST_PushHistory(BYTE cpu, BYTE load, BYTE fan);


// HistIdx — ring buffer index helper (used by DrawGraph in StressTestCPU)
static inline int ST_HistIdx(int fromOldest)
{
    return (s_histHead + fromOldest) % HISTORY_LEN;
}

// DrawTabStrip — defined in StressTest.cpp, called by RenderRAMCard
void ST_DrawTabStrip(StressCard active);

// ============================================================================
// Public API
// ============================================================================

void StressTest_OnEnter();
void StressTest_Tick(const DiagLogo& logo);
void StressTest_AutoRun(HANDLE hReport, DWORD durationMs);

// CPU AutoRun result accessors
BYTE  StressAutoRun_GetMinCPU();
BYTE  StressAutoRun_GetMaxCPU();
BYTE  StressAutoRun_GetMinFan();
BYTE  StressAutoRun_GetMaxFan();
BYTE  StressAutoRun_GetMeasuredLoad();
bool  StressAutoRun_GetThermalAbort();