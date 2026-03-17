#pragma once
// EepromSettings.h
// XbDiag - EEPROM settings editor (VIDEO / AUDIO / REGION / TIME cards).

#include "DiagCommon.h"

void EepromSettings_Load();
void EepromSettings_RecalcChecksums(BYTE* buf);
void EepromSettings_GetUtcTzBlock(BYTE out[44]); // fill out with UTC+0 TZ entry raw bytes  // recalculate both checksums into buf                              // load working copies from s_eeprom
void EepromSettings_HandleInput(WORD cur, WORD prev);    // process input when VIEW_EDIT active
void EepromSettings_Render(const DiagLogo& logo);        // render the edit view