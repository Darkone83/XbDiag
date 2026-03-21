#pragma once
// HddBench.h
// XbDiag - HDD Benchmark view.
// Reads drive metadata from s_data (HddInfo.h) and switches s_view on exit.

#include "DiagCommon.h"
#include <xtl.h>


// ============================================================================
// Bench state - exposed so HddInfo_Tick and AutoRun can check/set state
// ============================================================================

enum BenchState
{
    BENCH_IDLE = 0,
    BENCH_CONFIRM,
    BENCH_WRITE,
    BENCH_RAW_WR,
    BENCH_READ,
    BENCH_CACHE_RD,
    BENCH_4K_RAND,
    BENCH_SEEK,
    BENCH_DONE
};

struct BenchData
{
    BenchState  state;
    bool        readOnly;
    char        readSrc[64];
    HANDLE      hWrite;
    DWORD       writeTotal;
    DWORD       writeT0;
    float       writeMBs;
    HANDLE      hRawWr;
    DWORD       rawWrTotal;
    DWORD       rawWrT0;
    float       rawWrMBs;
    HANDLE      hRead;
    DWORD       readTotal;
    DWORD       readT0;
    float       readMBs;
    HANDLE      hCache;
    int         cachePass;
    DWORD       cacheTotal;
    DWORD       cacheT0;
    float       cacheMBs;
    HANDLE      hRand4k;
    int         rand4kIdx;
    DWORD       rand4kT0;
    float       rand4kMBs;
    float       rand4kIOPS;
    HANDLE      hSeek;
    int         seekIdx;
    DWORD       seekT0;
    float       seekMs;
    bool        tmpExists;
    bool        exportDone;
    bool        exportOK;
    char        statusMsg[80];
    DWORD       nextRender;
};

void HddBench_Start();
void HddBench_Cleanup();
void HddBench_Tick();
void HddBench_Render(const DiagLogo& logo);
void HddBench_Export();
bool HddBench_IsRunning();   // true while a benchmark phase is in progress

// Internal bench state accessor for HddInfo_AutoRun
BenchData& HddBench_GetData();
void HddBench_Reset();   // cleanup + zero — call from OnEnter