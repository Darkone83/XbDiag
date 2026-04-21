#pragma once
// StressTestCPU.h
// XbDiag - CPU stress engine (FPU torture, telemetry, render).

#include "DiagCommon.h"

void ST_CPU_TakeSample();               // read CPU/MB temp + fan via SMBus
void ST_CPU_ReadMHz();                  // update s_curMHz from PCI PLL
void ST_CPU_Render(const DiagLogo& logo); // render the CPU card view

// Fan speed overlay — called from StressTest.cpp when SSTATE_FAN is active.
// StressTest.h must include SSTATE_FAN in the StressState enum.
void ST_CPU_FanToggleAuto();            // toggle between AUTO and MANUAL modes
void ST_CPU_FanStep(int delta);         // +1 or -1 (one 5% step), MANUAL only
void ST_CPU_FanApply();                 // write target to PIC reg 0x06 and exit
void ST_CPU_FanCancel();                // cancel — revert to auto, no PIC write
void ST_CPU_FanRelease();              // module exit — restore SMC control
bool ST_CPU_FanIsAuto();                // query current mode for input gating
int  ST_CPU_FanGetPct();               // query current target percentage