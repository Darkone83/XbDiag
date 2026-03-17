#pragma once
#include <xtl.h>

// Simple 5x7 bitmap font renderer.
// Uses the global g_pDevice defined in main.cpp.
void DrawText(float x, float y, const char* text, float scale, DWORD color);

// Call once after video mode detection.
// isSD = true for 480i and 576i PAL — enables SD readability improvements:
//   - Scanline-safe pixel height (1.5x vertical bloat per dot)
//   - Wider letter spacing (6.5 vs 6.0 design pixels per char)
//   - Separated, alpha-blended drop shadow
void Font_SetSD(bool isSD);

// Returns the current per-character advance in design pixels at scale 1.0.
// DiagCommon's TW() uses this to keep measurements in sync with rendering.
float Font_GetAdvance();