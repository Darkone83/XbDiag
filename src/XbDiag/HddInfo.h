#pragma once
// HddInfo.h
// XbDiag - HDD Info module public interface.
//
// HddData and HddView are defined here and exported as externs so HddSmart
// and HddBench can access drive data and switch views without coupling.

#include "DiagCommon.h"

// ============================================================================
// Shared data structures
// ============================================================================

struct HddData
{
    // ATA drive info
    bool  ataOK;
    char  model[44];
    char  serial[22];
    char  fwRev[10];
    char  capacity[16];
    char  udmaMode[10];
    char  bufferKB[10];
    char  lba28sectors[16];
    char  lba48sectors[20];
    bool  lba48supported;
    bool  isLocked;
    bool  securitySupported;
    bool  secEnabled;          // security feature enabled (bit 1 of word 128)
    bool  isSSD;
    char  rpmStr[12];
    char  ataVersion[8];
    WORD  identBuf[256];

    // EEPROM security data
    bool  eepromOK;
    bool  eepromDecrypted;
    BYTE  hddKey[16];
    BYTE  xbeRegion[4];
    BYTE  onlineKey[16];
    BYTE  regionByte;
    char  regionStr[24];
    char  serialEEPROM[14];

    // Partitions (C/E/F/G)
    struct PartInfo {
        char   letter;
        bool   present;
        char   totalStr[12];
        char   freeStr[12];
        float  freeRatio;
        int    clusterKB;   // cluster size in KB (0 = unknown)
        bool   clusterOK;   // false if undersized for partition total
    } parts[4];

    // Export
    bool  exportDone;
    bool  exportOK;

    // SMART
    bool  smartSupported;
    bool  smartOK;
    bool  smartExportDone;
    bool  smartExportOK;
    BYTE  smartBuf[512];
};

// View state — shared so Smart and Bench can switch back to INFO
enum HddView { VIEW_INFO = 0, VIEW_SMART, VIEW_BENCH };

// Globals defined in HddInfo.cpp, referenced by HddSmart and HddBench
extern HddData s_data;
extern HddView s_view;

// ============================================================================
// Public API
// ============================================================================

void HddInfo_OnEnter();
void HddInfo_Tick(const DiagLogo& logo);
void HddInfo_AutoRun(HANDLE hReport);