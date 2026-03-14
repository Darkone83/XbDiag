#pragma once
#include "DiagCommon.h"

// ============================================================================
// XbSet — Automation settings and autorun engine
//
// Settings file:  D:\XbDiag.set   (created/edited via XbSet UI)
// Report file:    E:\XbDiag.txt   (written during autorun)
//
// Hidden menu: White+Black simultaneously from the main menu.
//
// Autorun on launch: if XbDiag.set exists, a 5-second countdown is shown.
// B cancels. If it expires, all enabled modules run headlessly and results
// are written to E:\XbDiag.txt.
//
// Stress tests run for a configurable duration (30s / 60s / 120s / 300s).
// Modules requiring user input (ControllerTest) wait up to 60 seconds for
// input — if no input arrives the test is skipped and noted in the report.
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

    // Stress duration in hours + minutes (0h 01m .. 99h 59m)
    int  stressHours;   // 0-99
    int  stressMins;    // 0-59  (minimum 1 min if hours==0)
};

extern AutoSettings g_autoSettings;
extern bool         g_autoSettingsFound;

bool XbSet_LoadSettings();
bool XbSet_SaveSettings();
bool XbSet_DeleteSettings();  // Deletes XbDiag.set, disables autorun
void XbSet_AutoRun(const DiagLogo& logo);

void XbSet_OnEnter();
void XbSet_Tick(const DiagLogo& logo);