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
//   Normal:      CPU stress runs for cpuStress duration, then RAM stress,
//                then GPU stress. Sequence repeated stressLoops times total.
//   Alt (interleaved): CPU/RAM/GPU loop alternating, stressLoops times.
//   Loop count:  1-99. Each loop runs the full CPU+RAM+GPU sequence.
//
// Shutdown after completion: calls PIC16L power-off command after report.
//
// Controller test runs as an automated snapshot -- no user interaction required.
// Records port/MU presence, stick positions at rest, and stuck buttons.
// For in-depth testing use the interactive Controller Test module directly.
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

    // Automated snapshot -- no user input required
    bool runCtrlTest;

    // Stress tests
    bool runCpuStress;
    bool runRamStress;
    bool runGpuStress;

    // Per-stress durations (independent)
    int  cpuStressHours;   // 0-99
    int  cpuStressMins;    // 0-59  (min 1 min if hours==0)
    int  ramStressHours;   // 0-99
    int  ramStressMins;    // 0-59  (min 1 min if hours==0)
    int  gpuStressHours;   // 0-99
    int  gpuStressMins;    // 0-59  (min 1 min if hours==0)

    // Stress loop count: run the CPU+RAM+GPU stress sequence this many times
    int  stressLoops;      // 1-99

    // Alt stress mode: interleave CPU/RAM/GPU loops
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