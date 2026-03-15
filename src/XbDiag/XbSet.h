#pragma once
#include "DiagCommon.h"

// ============================================================================
// XbSet — Automation settings and autorun engine
//
// Settings file:  D:\XbDiag.set   (created/edited via XbSet UI)
// Report file:    D:\XbDiag.txt   (written during autorun)
//
// Hidden menu: Back+White chord simultaneously from the main menu.
//
// Autorun on launch: if XbDiag.set exists, a 30-second countdown is shown.
// [B] cancels. If the countdown expires, all enabled modules run headlessly
// and results are written to D:\XbDiag.txt.
//
// Stress test modes:
//   Normal:      CPU stress runs for cpuStress duration, then RAM stress.
//                Sequence repeated stressLoops times total.
//   Alt (interleaved): CPU loop then RAM loop alternating, stressLoops times.
//   Loop count:  1-99. Each loop runs the full CPU+RAM sequence.
//
// Shutdown after completion: calls HalReturnToFirmware(HalPowerDownRoutine)
// after the report is written.
//
// Controller test waits up to 60 seconds for [A] to confirm presence.
// If no input arrives the test is skipped and noted in the report.
// ============================================================================

struct AutoSettings
{
    // Non-interactive modules
    bool runSysInfo;
    bool runVideoInfo;
    bool runSmBus;
    bool runHddInfo;
    bool runRamTest;
    bool runTempMon;
    bool runEeprom;

    // Interactive — waits up to 60s for input, skips if none
    bool runCtrlTest;

    // Stress tests
    bool runCpuStress;
    bool runRamStress;

    // Per-stress durations (independent)
    int  cpuStressHours;   // 0-99
    int  cpuStressMins;    // 0-59  (min 1 min if hours==0)
    int  ramStressHours;   // 0-99
    int  ramStressMins;    // 0-59  (min 1 min if hours==0)

    // Stress loop count: run the CPU+RAM stress sequence this many times
    int  stressLoops;      // 1-99

    // Alt stress mode: interleave CPU and RAM per loop
    // false = all CPU loops then all RAM loops
    // true  = CPU, RAM, CPU, RAM, ... per loop count
    bool altStressMode;

    // Shutdown Xbox after autorun report is written
    bool shutdownAfter;
};

extern AutoSettings g_autoSettings;
extern bool         g_autoSettingsFound;

bool XbSet_LoadSettings();
bool XbSet_SaveSettings();
bool XbSet_DeleteSettings();
void XbSet_AutoRun(const DiagLogo& logo);

void XbSet_OnEnter();
void XbSet_Tick(const DiagLogo& logo);