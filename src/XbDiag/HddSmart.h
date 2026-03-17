#pragma once
// HddSmart.h
// XbDiag - HDD SMART view.
// Reads from s_data (defined in HddInfo.cpp, declared in HddInfo.h).

#include "DiagCommon.h"

void HddSmart_Render(const DiagLogo& logo);
void HddSmart_Export();