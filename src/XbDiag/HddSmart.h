#pragma once
// HddSmart.h
// XbDiag - HDD SMART view.
// Reads from s_data (defined in HddInfo.cpp, declared in HddInfo.h).

#include "DiagCommon.h"

void HddSmart_Render(const DiagLogo& logo);
void HddSmart_Export();
void HddSmart_ResetScroll();  // call from HddInfo when switching to SMART tab

// HTTP server accessors — expose decoded rows without duplicating the DB.
// Rows are populated the first time HddSmart_Render runs after HddInfo loads.
// Returns 0 if SMART data not yet available (HddInfo not run this session).
int  HddSmart_GetRowCount();
bool HddSmart_GetRow(int idx,
    char* idBuf, int idLen,    // e.g. "C5"
    char* nameBuf, int nameLen,  // e.g. "Current Pending Sectors"
    char* valBuf, int valLen,   // decoded e.g. "0 sct" or "43C (22/55)"
    bool* outCrit,               // true = critical attribute
    bool* outTripped);           // true = cur <= thr (threshold tripped)

// Returns raw normalised cur/wst/thr for a row (0-255 normalised values)
bool HddSmart_GetRowRaw(int idx, int* cur, int* wst, int* thr);