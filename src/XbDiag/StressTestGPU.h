#pragma once
// StressTestGPU.h
// XbDiag - GPU Stress Test card (CARD_GPU)
//
// CrystalScene (Crystalline Grotto) is inlined directly into StressTestGPU.cpp.
// No CrystalScene.h or CrystalScene.cpp dependency — fully self-contained.
//
// Stress card behaviour:
//   Scene loops every 20s until [Back]+[A] held 5s (same UX as CPU/RAM).
//   Overlay strip: ELAPSED / LOOPS / FPS / PEAK / MIN / CPU / FAN.

#include "DiagCommon.h"

void ST_GPU_OnStart();
void ST_GPU_Stop();
void ST_GPU_Render(const DiagLogo& logo);

// XbSet AutoRun — headless GPU stress with report output
void  GpuStress_AutoRun(HANDLE hReport, DWORD durationMs);
DWORD GpuAutoRun_GetLoops();
float GpuAutoRun_GetPeakFPS();
float GpuAutoRun_GetMinFPS();