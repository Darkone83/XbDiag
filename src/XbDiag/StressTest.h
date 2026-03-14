#pragma once
// StressTest.h
// CPU / GPU / RAM sustained stress test module for XbDiag.

#include "DiagCommon.h"

void StressTest_OnEnter();
void StressTest_Tick(const DiagLogo& logo);
void StressTest_AutoRun(HANDLE hReport, DWORD durationMs);
void RamStress_AutoRun(HANDLE hReport, DWORD durationMs);