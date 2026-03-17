#pragma once
// StressTestCPU.h
// XbDiag - CPU stress engine (FPU torture, telemetry, render).

#include "DiagCommon.h"

void ST_CPU_TakeSample();               // read CPU/MB temp + fan via SMBus
void ST_CPU_ReadMHz();                  // update s_curMHz from PCI PLL
void ST_CPU_Render(const DiagLogo& logo); // render the CPU card view