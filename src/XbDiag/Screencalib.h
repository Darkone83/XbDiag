// ============================================================================
// ScreenCalib.h — Runtime screen calibration / overscan adjustment
// ============================================================================
//
// Provides a first-boot calibration screen that lets the user adjust the four
// screen margins to compensate for CRT overscan. Settings are saved to
// D:\screen.set and loaded automatically on subsequent boots.
//
// If running from disc (ISO) or if the write fails for any reason, settings
// are stored in RAM for the current session only — the app continues normally
// without blocking.
//
// Call order in main.cpp:
//   ScreenCalib_Init()         — load screen.set, populate g_marginL/R/T/B
//   if (ScreenCalib_NeedsRun()) ScreenCalib_Run()  — first boot only
//   Update_StartBootCheck()    — existing boot sequence
//
// Re-calibration: call ScreenCalib_Run() at any time (e.g. from menu).
// ============================================================================

#pragma once
#include "DiagCommon.h"
#include <xtl.h>

// ── Runtime margin globals ────────────────────────────────────────────────────
// Initialised from screen.set, or to compile-time defaults if not present.
// DrawPageChrome and DiagMenu use these instead of the LM/BOT_BAR_Y constants
// so calibration affects all chrome elements without touching module content.
extern float g_marginL;    // left margin  (default = LM     = 48.f)
extern float g_marginR;    // right margin (default = LM     = 48.f)
extern float g_marginT;    // top bar base (default = CONTENT_Y = 58.f)
extern float g_marginB;    // bottom bar Y (default = BOT_BAR_Y = 416.f)

// ── Calibration range ────────────────────────────────────────────────────────
// User can adjust each margin by up to ±32px from its compile-time default.
// Fine step = 1px, coarse step = 4px.
#define CALIB_STEP_FINE    1.f
#define CALIB_STEP_COARSE  4.f
#define CALIB_MARGIN_MIN   0.f     // 0 = brackets at absolute screen edge
#define CALIB_MARGIN_MAX   80.f
#define CALIB_BOT_MIN     (BOT_BAR_Y - 36.f)
#define CALIB_BOT_MAX     (BOT_BAR_Y + 36.f)
#define CALIB_TOP_MIN     (CONTENT_Y - 10.f)
#define CALIB_TOP_MAX     (CONTENT_Y + 14.f)

// ── Public API ────────────────────────────────────────────────────────────────

// Load screen.set and populate g_margin* globals. Call once at boot before
// anything renders. Safe to call if file does not exist — uses defaults.
void ScreenCalib_Init(const DiagLogo& logo);

// Returns true if no screen.set was found on this boot and calibration
// should run before the update check.
bool ScreenCalib_NeedsRun();

// Run the interactive calibration screen. Blocks until user confirms [A]
// or cancels [B]. Saves screen.set on confirm (silently skips if write fails).
// Safe to call at any time — e.g. from main menu [BLACK].
void ScreenCalib_Run(const DiagLogo& logo);

// Returns true if screen.set could not be written (disc / read-only media).
// Used to show a brief notice to the user.
bool ScreenCalib_IsReadOnly();