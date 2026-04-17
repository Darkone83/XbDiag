#pragma once
#include "DiagCommon.h"
#include "lcd.h"

// Snapshot struct for the HTTP report server — all fields are pointers into
// the static SysData block inside SysInfo.cpp. Valid only while the module
// is loaded; copy strings before calling SysInfo_OnEnter() again.
struct SysSnapshot
{
    const char* cpuIC;
    const char* cpuSpeed;
    const char* cpuBrand;
    const char* memTotal;
    const char* memConfig;
    const char* boardRev;
    const char* serialNum;
    const char* modchip;
    const char* hdMod;
    const char* biosVer;
    const char* encName;
    const char* avPack;
    const char* tempCPU;
    const char* tempAmbient;
    const char* hddModel;
    const char* hddSize;
    const char* hddUDMA;
    const char* macAddr;
    const char* ipAddr;
    const char* gpuSpeed;
};

void SysInfo_OnEnter();
void SysInfo_Tick(const DiagLogo& logo);
const char* SysInfo_GetBoardRev();
void SysInfo_GetLCDData(LCDData& out);
bool SysInfo_GetSnapshot(SysSnapshot& out);
void SysInfo_AutoRun(HANDLE hReport);