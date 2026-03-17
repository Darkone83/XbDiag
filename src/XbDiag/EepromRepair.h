#pragma once
// EepromRepair.h
// XbDiag - EEPROM repair and restore.

#include "DiagCommon.h"

// Shared write/checksum helpers also used by EepromSettings
DWORD EepRepair_CalcChecksum(const BYTE* data, int offset, int size);
bool  EepRepair_SMBusWriteDW(int offset, DWORD val);

void EepromRepair_BuildDiag();                                       // build diagnostic item list from s_eeprom
void EepromRepair_HandleInput(WORD cur, WORD prev, const DiagLogo& logo);
void EepromRepair_Render(const DiagLogo& logo);