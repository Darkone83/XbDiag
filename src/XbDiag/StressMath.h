#pragma once
// =============================================================================
// StressMath.h
// =============================================================================

// ── Tuning constants ──────────────────────────────────────────────────────────
//
// MEM_BURST_CYCLES — number of compute iterations between MemFlood bursts.
//   Default 6 targets ~60% compute / ~40% memory flood at 733 MHz.
//   Lower = more memory/FSB pressure (minimum useful value: 4).
//   Higher = more compute-dominant (10+ shifts back toward core-only burn).
//   Recommended tuning steps: 6 → 4 (more heat) / 8 (less bursty feel).
//
// MEM_BURST_MS — duration of each MemFlood burst in milliseconds.
//   Default 60ms. Raise to 75ms for more sustained FSB pressure.
//   Lower to 40ms if the burst feels too long on a Tualatin upgrade.

static const int MEM_BURST_CYCLES = 6;
static const int MEM_BURST_MS = 60;

// ── Public API ────────────────────────────────────────────────────────────────

// Call once from StressTest_OnEnter.
void StressMath_Init();

// Burn CPU (x87 + SSE1 + integer) until GetTickCount() >= deadline.
// Every MEM_BURST_CYCLES iterations fires a MEM_BURST_MS MemFlood burst
// to broaden thermal soak to FSB/DRAM/northbridge.
void CPUStress(DWORD deadline);

// Stress DRAM bandwidth until GetTickCount() >= deadline.
// 2MB buffer — well outside L2 on all Xbox CPU variants.
void MemFlood_Timed(DWORD deadline);

// Returns true if SSE1 XMM registers verified working during Init.
bool StressMath_SSEEnabled();