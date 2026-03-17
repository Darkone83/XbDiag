#pragma once
// StressTestRAM.h
// XbDiag - RAM stress engine (moving inversions soak, 11 phases).

#include "DiagCommon.h"

void ST_RAM_OnStart();                      // initialise RAM stress state
void ST_RAM_Stop();                         // release allocated memory
void ST_RAM_Step();                         // advance one time-sliced tick
void ST_RAM_Render(const DiagLogo& logo);   // render the RAM card view

void RamStress_AutoRun(HANDLE hReport, DWORD durationMs);

// RAM AutoRun result accessors
DWORD RamAutoRun_GetSweeps();
DWORD RamAutoRun_GetErrors();
int   RamAutoRun_GetFailed();