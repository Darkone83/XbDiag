#pragma once
#include "DiagCommon.h"
#include "lcd.h"
void SysInfo_OnEnter();
void SysInfo_Tick(const DiagLogo& logo);
const char* SysInfo_GetBoardRev();
void SysInfo_GetLCDData(LCDData& out);
void SysInfo_AutoRun(HANDLE hReport);