#pragma once
// =============================================================================
// StressMath.h
//
// CPU stress kernel for XbDiag StressTest.
// Implementation lives entirely in StressMath.cpp — callers need nothing else.
// =============================================================================

// Call once from StressTest_OnEnter.
// Sets sm_sqrthalf = sqrt(0.5) and seeds the working buffer with a non-zero
// LCG pattern so every butterfly arm does genuine non-trivial FP work.
void StressMath_Init();

// Burn CPU until GetTickCount() >= deadline.
// Sweeps sm_data[SM_N] repeatedly with the eight_reals_fft_cmn x87 butterfly
// from Prime95 gwnum/lucas.mac.  Call from the StressTest_Tick burn loop.
void CPUStress(DWORD deadline);