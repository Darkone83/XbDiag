#pragma once
// =============================================================================
// StressMath.h
// =============================================================================

// Call once from StressTest_OnEnter.
void StressMath_Init();

// Burn CPU (x87 + SSE1) until GetTickCount() >= deadline.
void CPUStress(DWORD deadline);

// Stress DRAM bandwidth until GetTickCount() >= deadline.
void MemFlood_Timed(DWORD deadline);

// Returns true if SSE1 XMM registers verified working during Init.
bool StressMath_SSEEnabled();