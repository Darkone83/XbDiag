#pragma once
// FileExplorerMU.h
// XbDiag - Memory Unit mounting and formatting.

#include "DiagCommon.h"

bool FE_MU_MountAll();           // mount HDD partitions + all inserted MUs; returns true if any MU mounted
bool FE_MU_Format(int port, int slot);  // format MU as FATX; returns true on success