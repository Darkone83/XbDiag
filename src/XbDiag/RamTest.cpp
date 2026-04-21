// RamTest.cpp
// XbDiag - Memory Test module
//
// Xbox RAM physical layout (NV2A northbridge):
//   64MB  config: 4 banks x 16MB  (single rank, one chip per bank)
//   128MB config: 4 banks x 32MB  (dual rank,   two chips per bank)
//
//   Bank 0:  0x00000000 - 0x0FFFFFFF  (base 16MB + optional upper 16MB)
//   Bank 1:  0x10000000 - 0x1FFFFFFF
//   Bank 2:  0x20000000 - 0x2FFFFFFF
//   Bank 3:  0x30000000 - 0x3FFFFFFF
//
// Each bank is subdivided into 2MB display chunks for fine-grained reporting.
// Grid displays 4 rows (one per physical bank), N columns (chunks per bank).
//
// TWO TEST MODES:
//
// Quick test  [A] - chunk-by-chunk sanity check:
//   Each 2MB chunk allocated, tested with 4 patterns, freed:
//     0xAA55AA55, 0x55AA55AA, walking 1s, addr XOR 0xDEADBEEF
//   Fast, catches stuck-bit and address decoder faults.
//   One chunk per tick — display stays live throughout.
//
// Stress soak [X]/[Y] - large-block moving inversions (memtest86-style):
//   Allocates the largest contiguous block possible per bank (up to full bank).
//   Runs moving inversions across the ENTIRE live allocation simultaneously:
//     Phase 1/6  WRITE    fill entire block forward   0xAA55AA55
//     Phase 2/6  READ/W   verify fwd + write inverse  0x55AA55AA
//     Phase 3/6  READ/W   verify bwd + write forward  0xAA55AA55
//     Phase 4/6  READ     final forward verify        0xAA55AA55
//     Phase 5/6  WRITE    fill with addr XOR 0xDEADBEEF
//     Phase 6/6  READ     verify addr XOR pattern
//   Time-sliced at ~150ms per tick — render loop stays live, watchdog safe.
//   Adjacent cells all live simultaneously — catches coupling faults and
//   marginal cells under sustained bus load that chunk tests cannot detect.
//   Errors bucketed by offset into 2MB display chunks — grid stays meaningful.
//
// State machine:
//   STATE_IDLE        - waiting for input
//   STATE_QUICK       - quick chunk-by-chunk test in progress
//   STATE_QUICK_DONE  - quick test complete, showing verdict
//   STATE_STRESS      - large-block moving inversions soak in progress
//   STATE_STRESS_DONE - soak complete, showing cumulative verdict

#include "RamTest.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

// ============================================================================
// Constants
// ============================================================================

#define CHUNK_SIZE           (2  * 1024 * 1024)
#define BANK_SIZE_STD        (16 * 1024 * 1024)
#define BANK_SIZE_EXT        (32 * 1024 * 1024)
#define NUM_BANKS            4
#define CHUNKS_PER_BANK_STD  8
#define CHUNKS_PER_BANK_EXT  16
#define MAX_CHUNKS_PER_BANK  16

#define PATTERN_A   0xAA55AA55UL
#define PATTERN_B   0x55AA55AAUL

// Time budget per tick for stress mode.  150ms keeps the render loop alive
// and prevents PIC watchdog starvation without being so short that the
// overhead per tick dominates test time.
#define STRESS_TICK_MS  150

// ============================================================================
// Types
// ============================================================================

enum ChunkState
{
    CHUNK_UNTESTED = 0,
    CHUNK_SKIPPED,
    CHUNK_TESTING,
    CHUNK_PASS,
    CHUNK_FAIL,
};

enum TestState
{
    STATE_IDLE = 0,
    STATE_QUICK,
    STATE_QUICK_DONE,
    STATE_STRESS,
    STATE_STRESS_DONE,
};

// Stress pass phases — shown live so the user sees exactly what the
// hardware is doing.  Colour-coded by operation type in the sub-bar.
enum StressPhase
{
    SPHASE_ALLOC = 0,    // attempting contiguous allocation
    SPHASE_P1_WRITE,     // write 0xAA55AA55 forward
    SPHASE_P2_READW,     // read+verify fwd, write inverse 0x55AA
    SPHASE_P3_READW,     // read+verify bwd, write 0xAA55 backward
    SPHASE_P4_READ,      // final forward read verify
    SPHASE_P5_WRITE,     // write addr XOR 0xDEADBEEF
    SPHASE_P6_READ,      // verify addr XOR
    SPHASE_FREE,         // commit results, free allocation
};

struct ChunkResult
{
    ChunkState state;
    DWORD      errorCount;
};

// ============================================================================
// Module state  (all file-scope — no statics inside functions)
// ============================================================================

static ChunkResult s_chunks[NUM_BANKS][MAX_CHUNKS_PER_BANK];
static int         s_chunksPerBank = CHUNKS_PER_BANK_STD;
static bool        s_is128MB = false;

// Shared counters (current sweep)
static int         s_testedCount = 0;
static int         s_passCount = 0;
static int         s_failCount = 0;
static int         s_skipCount = 0;
static DWORD       s_totalErrors = 0;
static DWORD       s_totalPhysMB = 0;
static DWORD       s_availPhysMB = 0;
static DWORD       s_usedMB = 0;

static TestState   s_testState = STATE_IDLE;
static WORD        s_prevBtns = GetButtons();  // seed to prevent held-button phantom edges on entry
static bool        s_skipFirstTick = true;

// CSV export state — reset each time a new test starts
static bool        s_exportDone = false;
static bool        s_exportOK = false;

// View toggle
enum RamView { VIEW_MAIN = 0, VIEW_CHIPHELP };
static RamView     s_view = VIEW_MAIN;

// Quick test position
static int         s_curBank = 0;
static int         s_curChunk = 0;
static const char* s_quickPass = "";

// Soak / stress globals
static DWORD       s_soakDurationMs = 0;
static DWORD       s_soakStartTick = 0;
static int         s_soakSweep = 0;
static DWORD       s_soakTotalErr = 0;

// Stress per-bank state
static int         s_stressBank = 0;
static StressPhase s_stressPhase = SPHASE_ALLOC;
static DWORD* s_stressBase = NULL;
static DWORD       s_stressDwords = 0;
static DWORD       s_stressOffset = 0;
static DWORD       s_stressBankErr = 0;

// ============================================================================
// Kernel exports
// ============================================================================

extern "C" PVOID __stdcall MmAllocateContiguousMemory(ULONG NumberOfBytes);
extern "C" VOID  __stdcall MmFreeContiguousMemory(PVOID BaseAddress);

// ============================================================================
// Cache flush — mandatory between write and read passes.
// Without this the CPU L2 hit masks bad DRAM cells entirely.
// wbinvd is ring-0 only; RXDK titles run as ring-0 so this is safe.
// ============================================================================

static __forceinline void FlushCache()
{
    __asm { wbinvd }
}

// ============================================================================
// ResetTest
// ============================================================================

static void ResetTest()
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);

    s_totalPhysMB = (DWORD)(ms.dwTotalPhys / (1024 * 1024));
    s_availPhysMB = (DWORD)(ms.dwAvailPhys / (1024 * 1024));
    s_usedMB = s_totalPhysMB - s_availPhysMB;
    s_is128MB = (s_totalPhysMB >= 100);
    s_chunksPerBank = s_is128MB ? CHUNKS_PER_BANK_EXT : CHUNKS_PER_BANK_STD;

    for (int b = 0; b < NUM_BANKS; ++b)
        for (int c = 0; c < MAX_CHUNKS_PER_BANK; ++c)
        {
            s_chunks[b][c].state = CHUNK_UNTESTED;
            s_chunks[b][c].errorCount = 0;
        }

    s_curBank = 0;
    s_curChunk = 0;
    s_quickPass = "";
    s_testedCount = 0;
    s_passCount = 0;
    s_failCount = 0;
    s_skipCount = 0;
    s_totalErrors = 0;
    s_exportDone = false;
    s_exportOK = false;
}

static void ResetStressBank()
{
    if (s_stressBase)
    {
        MmFreeContiguousMemory(s_stressBase);
        s_stressBase = NULL;
    }
    s_stressDwords = 0;
    s_stressOffset = 0;
    s_stressBankErr = 0;
    s_stressPhase = SPHASE_ALLOC;
}

// ============================================================================
// OnEnter
// ============================================================================

void RamTest_OnEnter()
{
    s_prevBtns = GetButtons();  // seed to prevent held-button phantom edges on entry
    s_skipFirstTick = true;

    if (s_stressBase)
    {
        MmFreeContiguousMemory(s_stressBase);
        s_stressBase = NULL;
    }

    ResetTest();
    s_testState = STATE_IDLE;
    s_view = VIEW_MAIN;
}

// ============================================================================
// Quick test step  -  one 2MB chunk per call
// ============================================================================

static void QuickTestStep()
{
    if (s_curBank >= NUM_BANKS)
    {
        s_testState = STATE_QUICK_DONE;
        return;
    }

    int bank = s_curBank;
    int chunk = s_curChunk;

    s_chunks[bank][chunk].state = CHUNK_TESTING;

    DWORD* p = (DWORD*)MmAllocateContiguousMemory(CHUNK_SIZE);
    if (!p)
    {
        s_chunks[bank][chunk].state = CHUNK_SKIPPED;
        s_skipCount++;
    }
    else
    {
        DWORD dwords = CHUNK_SIZE / sizeof(DWORD);
        DWORD errors = 0;

        // Pass 1: 0xAA55AA55
        s_quickPass = "1/4  WRITE   0xAA55AA55";
        for (DWORD i = 0; i < dwords; ++i) p[i] = PATTERN_A;
        FlushCache();
        s_quickPass = "1/4  VERIFY  0xAA55AA55";
        for (DWORD i = 0; i < dwords; ++i)
            if (p[i] != PATTERN_A) errors++;

        // Pass 2: 0x55AA55AA
        s_quickPass = "2/4  WRITE   0x55AA55AA";
        for (DWORD i = 0; i < dwords; ++i) p[i] = PATTERN_B;
        FlushCache();
        s_quickPass = "2/4  VERIFY  0x55AA55AA";
        for (DWORD i = 0; i < dwords; ++i)
            if (p[i] != PATTERN_B) errors++;

        // Pass 3: walking 1s
        s_quickPass = "3/4  WRITE   WALKING 1s";
        for (DWORD i = 0; i < dwords; ++i)
            p[i] = (1UL << (i & 31));
        FlushCache();
        s_quickPass = "3/4  VERIFY  WALKING 1s";
        for (DWORD i = 0; i < dwords; ++i)
            if (p[i] != (1UL << (i & 31))) errors++;

        // Pass 4: address XOR
        s_quickPass = "4/4  WRITE   ADDR XOR";
        for (DWORD i = 0; i < dwords; ++i) p[i] = i ^ 0xDEADBEEFUL;
        FlushCache();
        s_quickPass = "4/4  VERIFY  ADDR XOR";
        for (DWORD i = 0; i < dwords; ++i)
            if (p[i] != (i ^ 0xDEADBEEFUL)) errors++;

        s_quickPass = "";

        MmFreeContiguousMemory(p);

        s_chunks[bank][chunk].errorCount = errors;
        s_chunks[bank][chunk].state = (errors == 0) ? CHUNK_PASS : CHUNK_FAIL;
        s_testedCount++;
        s_totalErrors += errors;
        if (errors == 0) s_passCount++;
        else             s_failCount++;
    }

    s_curChunk++;
    if (s_curChunk >= s_chunksPerBank) { s_curChunk = 0; s_curBank++; }
    if (s_curBank >= NUM_BANKS) s_testState = STATE_QUICK_DONE;
}

// ============================================================================
// Stress test step  -  large-block moving inversions, time-sliced
//
// One call per tick.  Processes DWORDs until STRESS_TICK_MS elapses, then
// returns so the render loop can update the display and service the system.
//
// Error address bucketing:
//   chunk = offset_in_dwords / (CHUNK_SIZE / 4)
// Maps any error within the large contiguous allocation back to the correct
// 2MB display cell in the grid, capped to chunksPerBank-1.
// ============================================================================

static void StressTestStep()
{
    DWORD chunkDw = CHUNK_SIZE / sizeof(DWORD);

    // ---- ALLOC phase -------------------------------------------------------
    if (s_stressPhase == SPHASE_ALLOC)
    {
        for (int c = 0; c < s_chunksPerBank; ++c)
            s_chunks[s_stressBank][c].state = CHUNK_TESTING;

        ULONG bankBytes = (ULONG)(s_is128MB ? BANK_SIZE_EXT : BANK_SIZE_STD);
        ULONG tryBytes = bankBytes;
        s_stressBase = NULL;
        while (!s_stressBase && tryBytes >= (ULONG)CHUNK_SIZE)
        {
            s_stressBase = (DWORD*)MmAllocateContiguousMemory(tryBytes);
            if (!s_stressBase) tryBytes /= 2;
        }

        if (!s_stressBase)
        {
            for (int c = 0; c < s_chunksPerBank; ++c)
            {
                s_chunks[s_stressBank][c].state = CHUNK_SKIPPED;
                s_skipCount++;
            }
            s_stressPhase = SPHASE_FREE;
            return;
        }

        s_stressDwords = tryBytes / sizeof(DWORD);
        s_stressOffset = 0;
        s_stressBankErr = 0;
        s_stressPhase = SPHASE_P1_WRITE;
        return;
    }

    // ---- FREE / commit phase -----------------------------------------------
    if (s_stressPhase == SPHASE_FREE)
    {
        if (s_stressBase)
        {
            MmFreeContiguousMemory(s_stressBase);
            s_stressBase = NULL;
        }

        for (int c = 0; c < s_chunksPerBank; ++c)
        {
            if (s_chunks[s_stressBank][c].state == CHUNK_TESTING)
            {
                bool anyErr = (s_chunks[s_stressBank][c].errorCount > 0);
                s_chunks[s_stressBank][c].state = anyErr ? CHUNK_FAIL : CHUNK_PASS;
                s_testedCount++;
                if (anyErr) s_failCount++;
                else        s_passCount++;
            }
        }

        s_totalErrors += s_stressBankErr;
        s_stressBank++;

        if (s_stressBank >= NUM_BANKS)
        {
            s_soakSweep++;
            s_soakTotalErr += s_totalErrors;

            DWORD elapsed = GetTickCount() - s_soakStartTick;
            if (elapsed >= s_soakDurationMs)
            {
                s_testState = STATE_STRESS_DONE;
            }
            else
            {
                // Reset for next sweep
                for (int b = 0; b < NUM_BANKS; ++b)
                    for (int c = 0; c < MAX_CHUNKS_PER_BANK; ++c)
                    {
                        s_chunks[b][c].state = CHUNK_UNTESTED;
                        s_chunks[b][c].errorCount = 0;
                    }
                s_stressBank = 0;
                s_testedCount = 0;
                s_passCount = 0;
                s_failCount = 0;
                s_skipCount = 0;
                s_totalErrors = 0;
            }
        }

        s_stressPhase = SPHASE_ALLOC;
        s_stressOffset = 0;
        return;
    }

    // ---- Active pass phases — time-sliced ----------------------------------
    DWORD tickStart = GetTickCount();
    DWORD* p = s_stressBase;
    DWORD  total = s_stressDwords;
    DWORD  off = s_stressOffset;

    if (s_stressPhase == SPHASE_P1_WRITE)
    {
        // Write 0xAA55AA55 forward across entire block
        while (off < total)
        {
            p[off] = PATTERN_A;
            off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= STRESS_TICK_MS)
                break;
        }
        if (off >= total)
        {
            FlushCache();
            s_stressPhase = SPHASE_P2_READW;
            s_stressOffset = 0;
            return;
        }
    }
    else if (s_stressPhase == SPHASE_P2_READW)
    {
        // Read forward: verify 0xAA55, write inverse 0x55AA in same pass
        // Coupling stress: read is interleaved with write to adjacent cells
        while (off < total)
        {
            if (p[off] != PATTERN_A)
            {
                int c = (int)(off / chunkDw);
                if (c >= s_chunksPerBank) c = s_chunksPerBank - 1;
                s_chunks[s_stressBank][c].errorCount++;
                s_stressBankErr++;
            }
            p[off] = PATTERN_B;
            off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= STRESS_TICK_MS)
                break;
        }
        if (off >= total)
        {
            FlushCache();
            s_stressPhase = SPHASE_P3_READW;
            s_stressOffset = total - 1;
            return;
        }
    }
    else if (s_stressPhase == SPHASE_P3_READW)
    {
        // Read backward: verify 0x55AA, write 0xAA55 backward
        // Backward pass stresses different row address sequencing
        // off counts down; wraps to 0xFFFFFFFF as sentinel for completion
        while (off < total)
        {
            if (p[off] != PATTERN_B)
            {
                int c = (int)(off / chunkDw);
                if (c >= s_chunksPerBank) c = s_chunksPerBank - 1;
                s_chunks[s_stressBank][c].errorCount++;
                s_stressBankErr++;
            }
            p[off] = PATTERN_A;
            if (off == 0) { off = 0xFFFFFFFFUL; break; }
            off--;
            if ((off & 0x3FF) == 0x3FF && (GetTickCount() - tickStart) >= STRESS_TICK_MS)
                break;
        }
        if (off == 0xFFFFFFFFUL)
        {
            FlushCache();
            s_stressPhase = SPHASE_P4_READ;
            s_stressOffset = 0;
            return;
        }
    }
    else if (s_stressPhase == SPHASE_P4_READ)
    {
        // Final forward read verify — confirms backward write pass was correct
        while (off < total)
        {
            if (p[off] != PATTERN_A)
            {
                int c = (int)(off / chunkDw);
                if (c >= s_chunksPerBank) c = s_chunksPerBank - 1;
                s_chunks[s_stressBank][c].errorCount++;
                s_stressBankErr++;
            }
            off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= STRESS_TICK_MS)
                break;
        }
        if (off >= total)
        {
            s_stressPhase = SPHASE_P5_WRITE;
            s_stressOffset = 0;
            return;
        }
    }
    else if (s_stressPhase == SPHASE_P5_WRITE)
    {
        // Write unique address XOR pattern — each DWORD has a unique value
        // so any aliasing fault (two addresses same physical cell) is detected
        while (off < total)
        {
            p[off] = off ^ 0xDEADBEEFUL;
            off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= STRESS_TICK_MS)
                break;
        }
        if (off >= total)
        {
            FlushCache();
            s_stressPhase = SPHASE_P6_READ;
            s_stressOffset = 0;
            return;
        }
    }
    else if (s_stressPhase == SPHASE_P6_READ)
    {
        // Verify address XOR pattern
        while (off < total)
        {
            if (p[off] != (off ^ 0xDEADBEEFUL))
            {
                int c = (int)(off / chunkDw);
                if (c >= s_chunksPerBank) c = s_chunksPerBank - 1;
                s_chunks[s_stressBank][c].errorCount++;
                s_stressBankErr++;
            }
            off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= STRESS_TICK_MS)
                break;
        }
        if (off >= total)
        {
            s_stressPhase = SPHASE_FREE;
            s_stressOffset = 0;
            return;
        }
    }

    s_stressOffset = off;
}

// ============================================================================
// ExportRamResult  — writes D:\ramresult.csv
//
// Only valid to call when s_testState == STATE_QUICK_DONE or STRESS_DONE.
// CSV layout:
//   Header rows with test metadata
//   Column header: Bank,Chunk,StartMB,EndMB,State,Errors
//   One row per chunk (all banks, all chunks up to s_chunksPerBank)
//   Footer rows: summary totals
// ============================================================================

static void ExportRamResult()
{
    HANDLE hf = CreateFile(
        "D:\\ramresult.csv",
        GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hf == INVALID_HANDLE_VALUE)
    {
        s_exportDone = true;
        s_exportOK = false;
        return;
    }

    char line[160];
    DWORD written;

    // Helper lambda-style: write one line + CRLF
#define WLINE(s) do { \
    DWORD _len = 0; \
    const char* _p = (s); \
    while (_p[_len]) _len++; \
    WriteFile(hf, _p, _len, &written, NULL); \
    WriteFile(hf, "\r\n", 2, &written, NULL); \
} while(0)

    // --- Metadata header ---
    WLINE("XbDiag RAM Test Result");

    bool isStress = (s_testState == STATE_STRESS_DONE);
    WLINE(isStress ? "Test Type,Stress Soak" : "Test Type,Quick");

    // Config
    StrCat2(line, sizeof(line), "RAM Config,",
        s_is128MB ? "128MB (4x32MB dual rank)" : "64MB (4x16MB single rank)");
    WLINE(line);

    // Total / avail
    char mbBuf[12];
    IntToStr((int)s_totalPhysMB, mbBuf, sizeof(mbBuf));
    StrCat3(line, sizeof(line), "Total MB,", mbBuf, "");
    WLINE(line);

    IntToStr((int)s_availPhysMB, mbBuf, sizeof(mbBuf));
    StrCat3(line, sizeof(line), "Available MB,", mbBuf, "");
    WLINE(line);

    // Stress-specific metadata
    if (isStress)
    {
        char swBuf[8];
        IntToStr(s_soakSweep, swBuf, sizeof(swBuf));
        StrCat3(line, sizeof(line), "Sweeps Completed,", swBuf, "");
        WLINE(line);

        char errBuf[16];
        UIntToStr(s_soakTotalErr, errBuf, sizeof(errBuf));
        StrCat3(line, sizeof(line), "Total Errors (all sweeps),", errBuf, "");
        WLINE(line);

        // Duration
        DWORD sec = s_soakDurationMs / 1000;
        char mm[4], ss[4];
        IntToStr((int)(sec / 60), mm, sizeof(mm));
        IntToStr((int)(sec % 60), ss, sizeof(ss));
        line[0] = 0;
        StrCat2(line, sizeof(line), line, "Soak Duration,");
        StrCat2(line, sizeof(line), line, mm);
        StrCat2(line, sizeof(line), line, "m ");
        StrCat2(line, sizeof(line), line, ss);
        StrCat2(line, sizeof(line), line, "s");
        WLINE(line);
    }

    WLINE("");

    // --- Column header ---
    WLINE("Bank,Chunk,Start MB,End MB,State,Errors");

    // --- Per-chunk rows ---
    DWORD chunkMB = s_is128MB ? 4 : 2;   // each chunk in MB
    for (int b = 0; b < NUM_BANKS; ++b)
    {
        for (int c = 0; c < s_chunksPerBank; ++c)
        {
            const ChunkResult& cr = s_chunks[b][c];

            const char* stateStr;
            switch (cr.state)
            {
            case CHUNK_PASS:     stateStr = "PASS";     break;
            case CHUNK_FAIL:     stateStr = "FAIL";     break;
            case CHUNK_SKIPPED:  stateStr = "SKIPPED";  break;
            case CHUNK_TESTING:  stateStr = "TESTING";  break;
            default:             stateStr = "UNTESTED"; break;
            }

            // Start/end MB within the full 64/128MB space
            DWORD bankBaseMB = (DWORD)b * (s_is128MB ? 32 : 16);
            DWORD startMB = bankBaseMB + (DWORD)c * chunkMB;
            DWORD endMB = startMB + chunkMB;

            char bBuf[4], cBuf[4], sMB[8], eMB[8], eBuf[12];
            IntToStr(b, bBuf, sizeof(bBuf));
            IntToStr(c, cBuf, sizeof(cBuf));
            IntToStr((int)startMB, sMB, sizeof(sMB));
            IntToStr((int)endMB, eMB, sizeof(eMB));
            UIntToStr(cr.errorCount, eBuf, sizeof(eBuf));

            line[0] = 0;
            StrCat2(line, sizeof(line), line, bBuf);
            StrCat2(line, sizeof(line), line, ",");
            StrCat2(line, sizeof(line), line, cBuf);
            StrCat2(line, sizeof(line), line, ",");
            StrCat2(line, sizeof(line), line, sMB);
            StrCat2(line, sizeof(line), line, ",");
            StrCat2(line, sizeof(line), line, eMB);
            StrCat2(line, sizeof(line), line, ",");
            StrCat2(line, sizeof(line), line, stateStr);
            StrCat2(line, sizeof(line), line, ",");
            StrCat2(line, sizeof(line), line, eBuf);
            WLINE(line);
        }
    }

    WLINE("");

    // --- Summary footer ---
    WLINE("--- Summary ---");

    char cntBuf[12];
    IntToStr(s_testedCount, cntBuf, sizeof(cntBuf));
    StrCat3(line, sizeof(line), "Chunks Tested,", cntBuf, "");
    WLINE(line);

    IntToStr(s_passCount, cntBuf, sizeof(cntBuf));
    StrCat3(line, sizeof(line), "Chunks Passed,", cntBuf, "");
    WLINE(line);

    IntToStr(s_failCount, cntBuf, sizeof(cntBuf));
    StrCat3(line, sizeof(line), "Chunks Failed,", cntBuf, "");
    WLINE(line);

    IntToStr(s_skipCount, cntBuf, sizeof(cntBuf));
    StrCat3(line, sizeof(line), "Chunks Skipped,", cntBuf, "");
    WLINE(line);

    UIntToStr(s_totalErrors, cntBuf, sizeof(cntBuf));
    StrCat3(line, sizeof(line), "Total Bit Errors,", cntBuf, "");
    WLINE(line);

    bool pass = (s_failCount == 0);
    WLINE(pass ? "Overall Result,PASS" : "Overall Result,FAIL");

    WLINE("");

    // --- Bank location guide ---
    WLINE("--- Physical Bank Identification ---");
    WLINE("RAM chips are arranged around the NV2A GPU (large center chip on the motherboard).");
    WLINE("Clockwise from front-left: U1 (Bank 0)  U2 (Bank 1)  U3 (Bank 2)  U4 (Bank 3).");
    WLINE("64MB:  4 chips total  1 chip per bank  (U1 U2 U3 U4).");
    WLINE("128MB: 8 chips total  2 chips per bank (U1A+U1B  U2A+U2B  U3A+U3B  U4A+U4B).");
    WLINE("128MB chunk mapping: chunks 0-7 = first chip (A)  chunks 8-15 = second chip (B).");
    WLINE("A failing bank row in the grid directly maps to the chip(s) listed above.");

#undef WLINE

    CloseHandle(hf);
    s_exportDone = true;
    s_exportOK = true;
}

// ============================================================================
// Input helper
// ============================================================================

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

// ============================================================================
// Phase label and colour for stress display
// ============================================================================

static const char* PhaseLabel(StressPhase ph)
{
    switch (ph)
    {
    case SPHASE_ALLOC:    return "ALLOCATING";
    case SPHASE_P1_WRITE: return "1/6  WRITE    fwd  0xAA55AA55";
    case SPHASE_P2_READW: return "2/6  READ+WRITE  fwd verify / inv write";
    case SPHASE_P3_READW: return "3/6  READ+WRITE  bwd verify / fwd write";
    case SPHASE_P4_READ:  return "4/6  READ     fwd verify 0xAA55AA55";
    case SPHASE_P5_WRITE: return "5/6  WRITE    addr XOR 0xDEADBEEF";
    case SPHASE_P6_READ:  return "6/6  READ     verify addr XOR";
    case SPHASE_FREE:     return "COMMITTING";
    default:              return "";
    }
}

// Sub-bar colour: orange=WRITE  cyan=READ+WRITE  green=READ/VERIFY
static DWORD PhaseBarColor(StressPhase ph)
{
    switch (ph)
    {
    case SPHASE_P1_WRITE:
    case SPHASE_P5_WRITE: return D3DCOLOR_XRGB(220, 120, 20);   // orange
    case SPHASE_P2_READW:
    case SPHASE_P3_READW: return D3DCOLOR_XRGB(20, 180, 220);  // cyan
    case SPHASE_P4_READ:
    case SPHASE_P6_READ:  return D3DCOLOR_XRGB(40, 200, 80);   // green
    default:              return D3DCOLOR_XRGB(80, 80, 80);   // gray
    }
}

static float PhaseProgress()
{
    if (s_stressDwords == 0) return 0.f;
    if (s_stressPhase == SPHASE_P3_READW && s_stressOffset < s_stressDwords)
    {
        DWORD done = s_stressDwords - 1 - s_stressOffset;
        return (float)done / (float)s_stressDwords;
    }
    return (float)s_stressOffset / (float)s_stressDwords;
}

// ============================================================================
// Chunk colour
// ============================================================================

static DWORD ChunkColor(ChunkState st, bool activeFlash)
{
    switch (st)
    {
    case CHUNK_PASS:    return COL_GREEN;
    case CHUNK_FAIL:    return COL_RED;
    case CHUNK_SKIPPED: return COL_GRAY;
    case CHUNK_TESTING: return activeFlash ? COL_YELLOW : D3DCOLOR_XRGB(200, 180, 0);
    default:            return COL_DIM;
    }
}

// ============================================================================
// RenderChipHelp  — dedicated chip identification card
// ============================================================================

static void RenderChipHelp(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    DrawPageChrome(logo, "MEMORY TEST  --  CHIP HELP", "[WHITE] / [B]  Back to Test");

    const float X = LM;
    const float STS = 1.15f;
    const float SLH = LINE_H - 3.f;
    const float DIAG_W = 340.f;          // width reserved for diagram column
    const float NX = X + DIAG_W;      // notes column start

    float y = CONTENT_Y + 6.f;

    // ---- Title + config ----------------------------------------------------
    DrawText(X, y, "CHIP IDENTIFICATION GUIDE", 1.3f, COL_YELLOW);
    y += LINE_H;
    DrawText(X, y, s_is128MB
        ? "CONFIG : 128MB  4 banks x 32MB  2 chips/bank  (8 chips total)"
        : "CONFIG : 64MB   4 banks x 16MB  1 chip/bank   (4 chips total)",
        STS, COL_CYAN);
    y += LINE_H;
    HLine(y, X, SW - LM, COL_BORDER);
    y += 6.f;

    // ---- Diagram (left) + Notes (right) ------------------------------------
    float dy = y;   // diagram y
    float ny = y;   // notes y

    // Diagram: chip boxes around GPU using FillRect
    const float CX = X + 160.f;
    const float BW = 60.f;
    const float BH = 16.f;
    const float GW = 80.f;
    const float GH = 60.f;
    const float GX = CX - GW * 0.5f;
    const float GAP = 18.f;

    // FRONT label
    DrawText(CX - 24.f, dy, "FRONT", STS, COL_DIM);
    dy += SLH;

    // U1 (top)
    FillRect(CX - BW * 0.5f, dy, CX + BW * 0.5f, dy + BH, D3DCOLOR_ARGB(80, 0, 180, 220));
    DrawText(CX - 22.f, dy + 1.f, "U1  Bank 0", STS, COL_CYAN);
    dy += BH + GAP;

    // Middle row
    float midY = dy;
    // U4 (left)
    FillRect(GX - GAP - BW, midY + (GH - BH) * 0.5f, GX - GAP, midY + (GH - BH) * 0.5f + BH,
        D3DCOLOR_ARGB(80, 0, 180, 220));
    DrawText(GX - GAP - BW + 4.f, midY + (GH - BH) * 0.5f + 1.f, "U4  B3", STS, COL_CYAN);
    // GPU box
    FillRect(GX, midY, GX + GW, midY + GH, D3DCOLOR_ARGB(50, 80, 80, 80));
    HLine(midY, GX, GX + GW, COL_BORDER);
    HLine(midY + GH, GX, GX + GW, COL_BORDER);
    VLine(GX, midY, midY + GH, COL_BORDER);
    VLine(GX + GW, midY, midY + GH, COL_BORDER);
    DrawText(GX + 12.f, midY + 16.f, "NV2A", STS, COL_DIM);
    DrawText(GX + 16.f, midY + 32.f, "GPU", STS, COL_DIM);
    // U2 (right)
    FillRect(GX + GW + GAP, midY + (GH - BH) * 0.5f, GX + GW + GAP + BW, midY + (GH - BH) * 0.5f + BH,
        D3DCOLOR_ARGB(80, 0, 180, 220));
    DrawText(GX + GW + GAP + 4.f, midY + (GH - BH) * 0.5f + 1.f, "U2  B1", STS, COL_CYAN);
    dy += GH + GAP;

    // U3 (bottom)
    FillRect(CX - BW * 0.5f, dy, CX + BW * 0.5f, dy + BH, D3DCOLOR_ARGB(80, 0, 180, 220));
    DrawText(CX - 22.f, dy + 1.f, "U3  Bank 2", STS, COL_CYAN);
    dy += BH + SLH;

    // REAR label
    DrawText(CX - 22.f, dy, "REAR", STS, COL_DIM);
    dy += SLH;

    // Notes (right column)
    DrawText(NX, ny, "RAM chips surround the NV2A GPU", STS, COL_WHITE);         ny += SLH;
    DrawText(NX, ny, "(large center chip on the board).", STS, COL_WHITE);        ny += SLH;
    DrawText(NX, ny, "U1 = front    U2 = right side", STS, COL_DIM);             ny += SLH;
    DrawText(NX, ny, "U3 = rear     U4 = left side", STS, COL_DIM);              ny += LINE_H;

    if (s_is128MB)
    {
        DrawText(NX, ny, "128MB: 2 chips per bank.", STS, COL_CYAN);              ny += SLH;
        DrawText(NX, ny, "Chunks 0-7  = A chip (inner)", STS, COL_WHITE);         ny += SLH;
        DrawText(NX, ny, "Chunks 8-15 = B chip (outer)", STS, COL_WHITE);         ny += LINE_H;
        DrawText(NX, ny, "To narrow down A vs B:", STS, COL_DIM);                 ny += SLH;
        DrawText(NX, ny, "check which chunk range has errors.", STS, COL_DIM);    ny += SLH;
    }
    else
    {
        DrawText(NX, ny, "64MB: 1 chip per bank.", STS, COL_CYAN);                ny += SLH;
        DrawText(NX, ny, "Any fail = that bank's chip.", STS, COL_WHITE);          ny += LINE_H;
        DrawText(NX, ny, "Multiple banks failing may", STS, COL_DIM);             ny += SLH;
        DrawText(NX, ny, "indicate NV2A or power issue.", STS, COL_DIM);           ny += SLH;
    }

    // Advance past whichever column is taller
    y = (dy > ny ? dy : ny) + 6.f;
    HLine(y, X, SW - LM, COL_BORDER);
    y += 5.f;

    // ---- Full-width bank table ---------------------------------------------
    const float TC_BNK = X;
    const float TC_ADDR = X + 46.f;
    const float TC_CHIP = X + 280.f;
    const float TC_LOC = X + 348.f;
    const float TC_DIAG = X + 490.f;

    DrawText(TC_BNK, y, "BNK", STS, COL_DIM);
    DrawText(TC_ADDR, y, "ADDRESS RANGE", STS, COL_DIM);
    DrawText(TC_CHIP, y, "CHIP(S)", STS, COL_DIM);
    DrawText(TC_LOC, y, "LOCATION", STS, COL_DIM);
    DrawText(TC_DIAG, y, "IF FAILING", STS, COL_DIM);
    y += SLH;
    HLine(y, X, SW - LM, D3DCOLOR_XRGB(40, 44, 80));
    y += 3.f;

    struct BankRow { const char* range; const char* c64; const char* c128; const char* loc; const char* d64; const char* d128; };
    BankRow rows[4];
    rows[0] = { "0x00000000 - 0x0FFFFFFF", "U1", "U1A+U1B", "Front / near drive", "Replace U1", "0-7=U1A 8-15=U1B" };
    rows[1] = { "0x10000000 - 0x1FFFFFFF", "U2", "U2A+U2B", "Right of GPU",       "Replace U2", "0-7=U2A 8-15=U2B" };
    rows[2] = { "0x20000000 - 0x2FFFFFFF", "U3", "U3A+U3B", "Rear of GPU",        "Replace U3", "0-7=U3A 8-15=U3B" };
    rows[3] = { "0x30000000 - 0x3FFFFFFF", "U4", "U4A+U4B", "Left of GPU",        "Replace U4", "0-7=U4A 8-15=U4B" };

    for (int b = 0; b < NUM_BANKS; ++b)
    {
        bool hasFail = false;
        for (int c = 0; c < s_chunksPerBank; ++c)
            if (s_chunks[b][c].state == CHUNK_FAIL) { hasFail = true; break; }

        if (hasFail || (b & 1))
            FillRect(X - 4.f, y - 1.f, SW - LM + 4.f, y + SLH + 2.f,
                hasFail ? D3DCOLOR_ARGB(35, 200, 20, 20) : D3DCOLOR_ARGB(18, 40, 60, 120));

        DWORD ac = hasFail ? COL_RED : COL_CYAN;
        DWORD dc = hasFail ? COL_RED : COL_DIM;
        char  bl[3] = { 'B', (char)('0' + b), 0 };

        DrawText(TC_BNK, y, bl, STS, ac);
        DrawText(TC_ADDR, y, rows[b].range, STS, COL_CYAN);
        DrawText(TC_CHIP, y, s_is128MB ? rows[b].c128 : rows[b].c64, STS, hasFail ? COL_RED : COL_WHITE);
        DrawText(TC_LOC, y, rows[b].loc, STS, dc);
        DrawText(TC_DIAG, y, s_is128MB ? rows[b].d128 : rows[b].d64, STS, dc);
        y += SLH + 2.f;
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// Render
// ============================================================================

static void Render(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    bool isRunning = (s_testState == STATE_QUICK || s_testState == STATE_STRESS);

    const char* hints;
    if (isRunning)
        hints = "[B] Abort";
    else if (s_testState == STATE_IDLE)
        hints = "[A] Quick    [X] 15min Stress    [Y] 30min Stress    [B] Back";
    else if (s_exportDone)
        hints = s_exportOK
        ? "[A] Quick    [X] 15min    [Y] 30min    [B] Back    Saved: D:\\ramresult.csv"
        : "[A] Quick    [X] 15min    [Y] 30min    [B] Back    Export FAILED";
    else
        hints = "[A] Quick    [X] 15min Stress    [Y] 30min Stress    [BLACK] Export    [B] Back";

    DrawPageChrome(logo, "MEMORY TEST", hints);

    const float COL_SPLIT = 300.f;
    const float MAP_LM = LM;
    const float GRID_LM = COL_SPLIT + 30.f;
    const float TS = 1.3f;
    const float LH = LINE_H - 2.f;
    const float VM_ = MAP_LM + 90.f;

    float y = CONTENT_Y + 6.f;

    // ---- Section headers ---------------------------------------------------
    DrawText(MAP_LM, y, "MEMORY MAP", TS, COL_YELLOW);
    DrawText(GRID_LM, y, "PHYSICAL BANK MAP", TS, COL_YELLOW);
    y += LH + 2.f;
    HLine(y, MAP_LM, COL_SPLIT - 8.f, COL_BORDER);
    HLine(y, GRID_LM, SW - LM, COL_BORDER);
    y += 5.f;

    // ---- Left: RAM summary -------------------------------------------------
    char buf[40];

    DrawText(MAP_LM, y, "CONFIG  :", TS, COL_GRAY);
    DrawText(VM_, y, s_is128MB ? "128MB" : "64MB", TS, COL_CYAN);
    y += LH;
    DrawText(VM_, y, s_is128MB ? "4x32MB  dual rank" : "4x16MB  single rank", TS, COL_CYAN);
    y += LH;

    IntToStr((int)s_totalPhysMB, buf, sizeof(buf));
    StrCat2(buf, sizeof(buf), buf, "MB");
    DrawText(MAP_LM, y, "TOTAL   :", TS, COL_GRAY);
    DrawText(VM_, y, buf, TS, COL_WHITE);
    y += LH;

    IntToStr((int)s_availPhysMB, buf, sizeof(buf));
    StrCat2(buf, sizeof(buf), buf, "MB");
    DrawText(MAP_LM, y, "AVAIL   :", TS, COL_GRAY);
    DrawText(VM_, y, buf, TS, COL_WHITE);
    y += LH;

    IntToStr((int)s_usedMB, buf, sizeof(buf));
    StrCat2(buf, sizeof(buf), buf, "MB");
    DrawText(MAP_LM, y, "IN USE  :", TS, COL_GRAY);
    DrawText(VM_, y, buf, TS, COL_ORANGE);
    y += LH + 4.f;

    // Bank address table
    HLine(y, MAP_LM, COL_SPLIT - 8.f, COL_BORDER);
    y += 5.f;
    DrawText(MAP_LM, y, "BANK", 1.15f, COL_DIM);
    DrawText(MAP_LM + 36.f, y, "BASE ADDR", 1.15f, COL_DIM);
    DrawText(MAP_LM + 122.f, y, "SIZE", 1.15f, COL_DIM);
    DrawText(MAP_LM + 162.f, y, "CHIPS", 1.15f, COL_DIM);
    y += LH;

    DWORD bankSizeMB = s_is128MB ? 32 : 16;
    for (int b = 0; b < NUM_BANKS; ++b)
    {
        bool active = (isRunning &&
            ((s_testState == STATE_QUICK && s_curBank == b) ||
                (s_testState == STATE_STRESS && s_stressBank == b)));
        DWORD rowCol = active ? COL_YELLOW : COL_WHITE;

        char bankBuf[4];
        IntToStr(b, bankBuf, sizeof(bankBuf));
        DrawText(MAP_LM, y, bankBuf, TS, active ? COL_CYAN : COL_GRAY);

        char addrBuf[12];
        IntToHex((DWORD)(b * (s_is128MB ? BANK_SIZE_EXT : BANK_SIZE_STD)),
            8, addrBuf, sizeof(addrBuf));
        char addrFull[12];
        StrCat2(addrFull, sizeof(addrFull), "0x", addrBuf);
        DrawText(MAP_LM + 36.f, y, addrFull, TS, COL_CYAN);

        char szBuf[8];
        IntToStr((int)bankSizeMB, szBuf, sizeof(szBuf));
        StrCat2(szBuf, sizeof(szBuf), szBuf, "MB");
        DrawText(MAP_LM + 122.f, y, szBuf, TS, rowCol);
        DrawText(MAP_LM + 162.f, y, s_is128MB ? "2" : "1", TS, rowCol);
        y += LH;
    }

    y += 4.f;
    HLine(y, MAP_LM, COL_SPLIT - 8.f, COL_BORDER);
    y += 5.f;

    // ---- Test status -------------------------------------------------------
    if (s_testState == STATE_IDLE)
    {
        DrawText(MAP_LM, y, "[A] Quick [X] 15min [Y] 30min", TS, COL_YELLOW);
    }
    else
    {
        // Status section uses tighter line height to fit all rows in the left panel
        const float SLH = LINE_H - 4.f;   // 14px — saves ~20px vs full LH

        // Overall progress bar (both modes)
        if (isRunning)
        {
            int totalChunks = NUM_BANKS * s_chunksPerBank;
            int doneChunks = (s_testState == STATE_QUICK)
                ? (s_curBank * s_chunksPerBank + s_curChunk)
                : (s_stressBank * s_chunksPerBank);

            const float BAR_W = COL_SPLIT - MAP_LM - 8.f;
            const float BAR_H = 8.f;
            float fillW = (totalChunks > 0)
                ? BAR_W * ((float)doneChunks / (float)totalChunks) : 0.f;

            FillRect(MAP_LM, y, MAP_LM + BAR_W, y + BAR_H, D3DCOLOR_XRGB(20, 25, 55));
            if (fillW > 0.f)
                FillRectGrad(MAP_LM, y, MAP_LM + fillW, y + BAR_H,
                    D3DCOLOR_XRGB(60, 160, 255), D3DCOLOR_XRGB(30, 90, 180));
            HLine(y, MAP_LM, MAP_LM + BAR_W, COL_BORDER);
            HLine(y + BAR_H, MAP_LM, MAP_LM + BAR_W, COL_BORDER);
            VLine(MAP_LM, y, y + BAR_H, COL_BORDER);
            VLine(MAP_LM + BAR_W, y, y + BAR_H, COL_BORDER);

            int  activeBank = (s_testState == STATE_QUICK) ? s_curBank : s_stressBank;
            char cbuf[4], tbuf[4];
            IntToStr(activeBank + 1, cbuf, sizeof(cbuf));
            IntToStr(NUM_BANKS, tbuf, sizeof(tbuf));
            char progBuf[24];
            StrCat3(progBuf, sizeof(progBuf), "Bank ", cbuf, " of ");
            StrCat2(progBuf, sizeof(progBuf), progBuf, tbuf);
            DrawText(MAP_LM, y + BAR_H + 2.f, progBuf, TS, COL_YELLOW);
            y += BAR_H + SLH + 2.f;
        }

        // Stress phase detail
        if (s_testState == STATE_STRESS)
        {
            // Phase label — kept short to fit between VM_ and COL_SPLIT
            DrawText(MAP_LM, y, "PHASE   :", TS, COL_GRAY);
            DrawText(MAP_LM + TW("PHASE   :", TS) + 4.f, y, PhaseLabel(s_stressPhase), 1.05f, COL_CYAN);
            y += SLH;

            // Phase sub-bar — colour by operation type, percent drawn inside bar
            float subProg = PhaseProgress();
            const float BAR_W = COL_SPLIT - MAP_LM - 8.f;
            const float BAR_H = 8.f;
            float fillW = BAR_W * subProg;
            DWORD barCol = PhaseBarColor(s_stressPhase);

            // Background
            FillRect(MAP_LM, y, MAP_LM + BAR_W, y + BAR_H, D3DCOLOR_XRGB(15, 18, 40));
            // Fill — minimum 2px pip so bar is always visibly alive even at 0%
            float drawW = fillW > 2.f ? fillW : 2.f;
            FillRect(MAP_LM, y, MAP_LM + drawW, y + BAR_H, barCol);
            // Border
            HLine(y, MAP_LM, MAP_LM + BAR_W, COL_BORDER);
            HLine(y + BAR_H, MAP_LM, MAP_LM + BAR_W, COL_BORDER);
            VLine(MAP_LM, y, y + BAR_H, COL_BORDER);
            VLine(MAP_LM + BAR_W, y, y + BAR_H, COL_BORDER);

            // Percent drawn inside bar right-aligned — always visible
            int pct = Ftoi(subProg * 100.f);
            char pctBuf[8];
            IntToStr(pct, pctBuf, sizeof(pctBuf));
            StrCat2(pctBuf, sizeof(pctBuf), pctBuf, "%");
            DrawText(MAP_LM + BAR_W - 28.f, y, pctBuf, 1.0f,
                D3DCOLOR_XRGB(220, 220, 220));
            y += BAR_H + 3.f;
        }

        // Soak timing (stress mode only)
        if (s_testState == STATE_STRESS || s_testState == STATE_STRESS_DONE)
        {
            DWORD elapsed = (s_testState == STATE_STRESS)
                ? (GetTickCount() - s_soakStartTick) : s_soakDurationMs;
            DWORD totalErr = s_soakTotalErr +
                (s_testState == STATE_STRESS ? s_totalErrors : 0);
            DWORD elSec = elapsed / 1000;
            DWORD remSec = (s_testState == STATE_STRESS && elapsed < s_soakDurationMs)
                ? (s_soakDurationMs - elapsed) / 1000 : 0;

            char elBuf[12];
            char mm[4], ss[4];
            IntToStr((int)(elSec / 60), mm, sizeof(mm));
            IntToStr((int)(elSec % 60), ss, sizeof(ss));
            elBuf[0] = 0;
            StrCat2(elBuf, sizeof(elBuf), elBuf, mm);
            StrCat2(elBuf, sizeof(elBuf), elBuf, "m ");
            StrCat2(elBuf, sizeof(elBuf), elBuf, ss);
            StrCat2(elBuf, sizeof(elBuf), elBuf, "s");

            DrawText(MAP_LM, y, "ELAPSED :", TS, COL_GRAY);
            DrawText(VM_, y, elBuf, TS, COL_WHITE);
            y += SLH;

            if (s_testState == STATE_STRESS)
            {
                char remBuf[12];
                char rm[4], rs[4];
                IntToStr((int)(remSec / 60), rm, sizeof(rm));
                IntToStr((int)(remSec % 60), rs, sizeof(rs));
                remBuf[0] = 0;
                StrCat2(remBuf, sizeof(remBuf), remBuf, rm);
                StrCat2(remBuf, sizeof(remBuf), remBuf, "m ");
                StrCat2(remBuf, sizeof(remBuf), remBuf, rs);
                StrCat2(remBuf, sizeof(remBuf), remBuf, "s");
                DrawText(MAP_LM, y, "REMAIN  :", TS, COL_GRAY);
                DrawText(VM_, y, remBuf, TS, COL_CYAN);
                y += SLH;
            }

            char swBuf[8];
            IntToStr(s_soakSweep, swBuf, sizeof(swBuf));
            DrawText(MAP_LM, y, "SWEEPS  :", TS, COL_GRAY);
            DrawText(VM_, y, swBuf, TS, COL_WHITE);
            y += SLH;

            UIntToStr(totalErr, buf, sizeof(buf));
            DrawText(MAP_LM, y, "TOT ERR :", TS, COL_GRAY);
            DrawText(VM_, y, buf, TS, totalErr > 0 ? COL_RED : COL_GREEN);
            y += SLH + 3.f;

            if (s_testState == STATE_STRESS_DONE)
            {
                HLine(y, MAP_LM, COL_SPLIT - 8.f, COL_BORDER);
                y += 4.f;
                DrawText(MAP_LM, y,
                    totalErr == 0 ? "STRESS: PASS" : "STRESS: FAIL",
                    1.4f, totalErr == 0 ? COL_GREEN : COL_RED);
                y += SLH;
            }
        }

        // Per-sweep results (both modes)
        IntToStr(s_testedCount, buf, sizeof(buf));
        DrawText(MAP_LM, y, "TESTED  :", TS, COL_GRAY);
        DrawText(VM_, y, buf, TS, COL_WHITE);
        y += SLH;

        IntToStr(s_passCount, buf, sizeof(buf));
        DrawText(MAP_LM, y, "PASSED  :", TS, COL_GRAY);
        DrawText(VM_, y, buf, TS, COL_GREEN);
        y += SLH;

        IntToStr(s_failCount, buf, sizeof(buf));
        DrawText(MAP_LM, y, "FAILED  :", TS, COL_GRAY);
        DrawText(VM_, y, buf, TS, s_failCount > 0 ? COL_RED : COL_DIM);
        y += SLH;

        IntToStr(s_skipCount, buf, sizeof(buf));
        DrawText(MAP_LM, y, "SKIPPED :", TS, COL_GRAY);
        DrawText(VM_, y, buf, TS, COL_GRAY);
        y += SLH;

        UIntToStr(s_totalErrors, buf, sizeof(buf));
        DrawText(MAP_LM, y, "ERRORS  :", TS, COL_GRAY);
        DrawText(VM_, y, buf, TS, s_totalErrors > 0 ? COL_RED : COL_GREEN);
        y += SLH + 3.f;

        if (s_testState == STATE_QUICK_DONE)
        {
            HLine(y, MAP_LM, COL_SPLIT - 8.f, COL_BORDER);
            y += 4.f;
            DrawText(MAP_LM, y,
                s_failCount == 0 ? "RESULT: PASS" : "RESULT: FAIL",
                1.4f, s_failCount == 0 ? COL_GREEN : COL_RED);
        }
    }

    // ---- Right: bank grid --------------------------------------------------
    {
        const float GRID_RIGHT = SW - 8.f;
        const float CELL_W = (GRID_RIGHT - GRID_LM) / (float)MAX_CHUNKS_PER_BANK - 2.f;
        const float CELL_H = 38.f;
        const float CELL_PAD = 2.f;
        const float ROW_PAD = 8.f;
        const float SLH = LINE_H - 2.f;
        bool        flash = ((GetTickCount() / 200) & 1) != 0;
        float       gridY = CONTENT_Y + 6.f + LH + 7.f;

        for (int b = 0; b < NUM_BANKS; ++b)
        {
            float rowY = gridY + (float)b * (CELL_H + ROW_PAD);

            char bankLbl[4];
            IntToStr(b, bankLbl, sizeof(bankLbl));
            char bankRow[8];
            StrCat2(bankRow, sizeof(bankRow), "B", bankLbl);
            bool bankActive = (isRunning &&
                ((s_testState == STATE_QUICK && s_curBank == b) ||
                    (s_testState == STATE_STRESS && s_stressBank == b)));
            DrawText(GRID_LM - 20.f, rowY + 10.f, bankRow, 1.2f,
                bankActive ? COL_YELLOW : COL_GRAY);

            for (int c = 0; c < s_chunksPerBank; ++c)
            {
                float cx = GRID_LM + (float)c * (CELL_W + CELL_PAD);
                // Flash only the active chunk in quick mode;
                // in stress mode the entire bank row is "active"
                bool isActive = (bankActive &&
                    s_testState == STATE_QUICK &&
                    s_curChunk == c);

                DWORD cellCol = ChunkColor(s_chunks[b][c].state, isActive && flash);
                FillRect(cx, rowY, cx + CELL_W, rowY + CELL_H, cellCol);

                char mbBuf[8];
                if (s_is128MB)
                    IntToStr(c, mbBuf, sizeof(mbBuf));
                else
                {
                    int mbStart = b * 16 + c * 2;
                    IntToStr(mbStart, mbBuf, sizeof(mbBuf));
                }
                DWORD lblCol = (s_chunks[b][c].state == CHUNK_PASS ||
                    s_chunks[b][c].state == CHUNK_SKIPPED)
                    ? COL_BG : D3DCOLOR_XRGB(40, 40, 40);
                DrawText(cx + 2.f, rowY + 4.f, mbBuf, 1.0f, lblCol);

                if (s_chunks[b][c].state == CHUNK_FAIL)
                {
                    char eBuf[8];
                    UIntToStr(s_chunks[b][c].errorCount, eBuf, sizeof(eBuf));
                    DrawText(cx + 2.f, rowY + CELL_H - 12.f, eBuf, 1.0f,
                        D3DCOLOR_XRGB(255, 200, 200));
                }
            }

            if (!s_is128MB)
            {
                for (int c = CHUNKS_PER_BANK_STD; c < MAX_CHUNKS_PER_BANK; ++c)
                {
                    float cx = GRID_LM + (float)c * (CELL_W + CELL_PAD);
                    FillRect(cx, rowY, cx + CELL_W, rowY + CELL_H,
                        D3DCOLOR_XRGB(12, 14, 28));
                    HLine(rowY + CELL_H * 0.5f - 1.f, cx + 3.f, cx + CELL_W - 3.f,
                        D3DCOLOR_XRGB(45, 45, 60));
                    VLine(cx + CELL_W * 0.5f, rowY + 3.f, rowY + CELL_H - 3.f,
                        D3DCOLOR_XRGB(45, 45, 60));
                }
            }
        }

        // Legend + phase key
        float legendY = gridY + (float)NUM_BANKS * (CELL_H + ROW_PAD) + 6.f;
        if (legendY < BOT_BAR_Y - 18.f)
        {
            // Chunk state legend
            struct LegItem { DWORD col; const char* label; };
            LegItem items[5];
            items[0].col = COL_GREEN;  items[0].label = "PASS";
            items[1].col = COL_RED;    items[1].label = "FAIL";
            items[2].col = COL_YELLOW; items[2].label = "TESTING";
            items[3].col = COL_GRAY;   items[3].label = "SKIP";
            items[4].col = COL_DIM;    items[4].label = "PENDING";

            float lx = GRID_LM;
            for (int i = 0; i < 5; ++i)
            {
                FillRect(lx, legendY + 3.f, lx + 10.f, legendY + 11.f, items[i].col);
                DrawText(lx + 13.f, legendY, items[i].label, 1.1f, COL_DIM);
                lx += 13.f + TW(items[i].label, 1.1f) + 8.f;
            }

            // Stress phase colour key
            float gy = legendY + 18.f;
            HLine(gy, GRID_LM, SW - 8.f, COL_BORDER);
            gy += 5.f;
            DrawText(GRID_LM, gy, "STRESS PHASE BAR", 1.2f, COL_YELLOW);
            gy += LINE_H;

            FillRect(GRID_LM, gy + 3.f, GRID_LM + 10.f, gy + 11.f, D3DCOLOR_XRGB(220, 120, 20));
            DrawText(GRID_LM + 13.f, gy, "WRITE", 1.1f, COL_DIM);
            FillRect(GRID_LM + 65.f, gy + 3.f, GRID_LM + 75.f, gy + 11.f, D3DCOLOR_XRGB(20, 180, 220));
            DrawText(GRID_LM + 78.f, gy, "READ+WRITE", 1.1f, COL_DIM);
            FillRect(GRID_LM + 175.f, gy + 3.f, GRID_LM + 185.f, gy + 11.f, D3DCOLOR_XRGB(40, 200, 80));
            DrawText(GRID_LM + 188.f, gy, "READ/VERIFY", 1.1f, COL_DIM);
            gy += LINE_H;

            if (s_is128MB)
            {
                DrawText(GRID_LM, gy,
                    "128MB: Chunks 0-7=CHIP1  8-15=CHIP2",
                    1.1f, COL_CYAN);
            }
            else
            {
                DrawText(GRID_LM, gy,
                    "64MB: 1 chip/bank  Fail=chip suspect",
                    1.1f, COL_CYAN);
            }
            gy += SLH + 2.f;
            DrawText(GRID_LM, gy,
                "[WHITE] Chip help  --  bank map + diag",
                1.05f, COL_YELLOW);
        }
    }

    VLine(COL_SPLIT, CONTENT_Y + 4.f, BOT_BAR_Y - 4.f, COL_BORDER);

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// Tick
// ============================================================================

void RamTest_Tick(const DiagLogo& logo)
{
    WORD cur = GetButtons();

    // Skip input on first tick — the button that navigated here may still be
    // held, which would fire a spurious edge and auto-start a test.
    if (s_skipFirstTick)
    {
        s_prevBtns = cur;
        s_skipFirstTick = false;
        Render(logo);
        return;
    }

    // [B] Abort running test or go back to menu
    if (EdgeDown(cur, s_prevBtns, BTN_B))
    {
        bool running = (s_testState == STATE_QUICK || s_testState == STATE_STRESS);
        if (running)
        {
            if (s_stressBase)
            {
                MmFreeContiguousMemory(s_stressBase);
                s_stressBase = NULL;
            }
            ResetTest();
            s_testState = STATE_IDLE;
        }
        else
        {
            RequestState(MSTATE_MENU);
        }
        s_prevBtns = cur;
        return;
    }

    // [WHITE] Toggle chip help card (available any time)
    if (EdgeDown(cur, s_prevBtns, BTN_WHITE))
    {
        s_view = (s_view == VIEW_MAIN) ? VIEW_CHIPHELP : VIEW_MAIN;
        s_prevBtns = cur;
        if (s_view == VIEW_CHIPHELP) { RenderChipHelp(logo); return; }
        Render(logo);
        return;
    }

    // On chip help card: only B or WHITE closes it — no other input processed
    if (s_view == VIEW_CHIPHELP)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_B))
            s_view = VIEW_MAIN;
        s_prevBtns = cur;
        RenderChipHelp(logo);
        return;
    }

    // [BLACK] Export CSV — only when a test is done and not yet exported
    if (EdgeDown(cur, s_prevBtns, BTN_BLACK))
    {
        bool done = (s_testState == STATE_QUICK_DONE || s_testState == STATE_STRESS_DONE);
        if (done && !s_exportDone)
            ExportRamResult();
    }

    // [A] Quick test
    if (EdgeDown(cur, s_prevBtns, BTN_A))
    {
        if (s_testState != STATE_QUICK && s_testState != STATE_STRESS)
        {
            ResetTest();
            s_soakSweep = 0;
            s_soakTotalErr = 0;
            s_testState = STATE_QUICK;
        }
    }

    // [X] 15 min stress
    if (EdgeDown(cur, s_prevBtns, BTN_X))
    {
        if (s_testState != STATE_QUICK && s_testState != STATE_STRESS)
        {
            ResetTest();
            ResetStressBank();
            s_stressBank = 0;
            s_soakDurationMs = 15 * 60 * 1000;
            s_soakStartTick = GetTickCount();
            s_soakSweep = 0;
            s_soakTotalErr = 0;
            s_testState = STATE_STRESS;
        }
    }

    // [Y] 30 min stress
    if (EdgeDown(cur, s_prevBtns, BTN_Y))
    {
        if (s_testState != STATE_QUICK && s_testState != STATE_STRESS)
        {
            ResetTest();
            ResetStressBank();
            s_stressBank = 0;
            s_soakDurationMs = 30 * 60 * 1000;
            s_soakStartTick = GetTickCount();
            s_soakSweep = 0;
            s_soakTotalErr = 0;
            s_testState = STATE_STRESS;
        }
    }

    s_prevBtns = cur;

    if (s_testState == STATE_QUICK)
        QuickTestStep();
    else if (s_testState == STATE_STRESS)
        StressTestStep();

    if (s_view == VIEW_CHIPHELP)
        RenderChipHelp(logo);
    else
        Render(logo);
}
// ============================================================================
// AutoRun — run one full quick test sweep, report results
// ============================================================================

void RamTest_AutoRun(HANDLE hReport)
{
    // Reset and start quick test
    ResetTest();
    s_testState = STATE_QUICK;
    s_curBank = 0;
    s_curChunk = 0;

    // Drive the quick test to completion synchronously
    while (s_testState == STATE_QUICK)
        QuickTestStep();

    char line[128]; DWORD w;
    if (!hReport || hReport == INVALID_HANDLE_VALUE) return;
    auto WL = [&](const char* lbl, const char* val)
        {
            StrCopy(line, sizeof(line), lbl);
            StrCat2(line, sizeof(line), line, val);
            StrCat2(line, sizeof(line), line, "\r\n");
            WriteFile(hReport, line, StrLen(line), &w, NULL);
        };

    char t[12];
    IntToStr((int)s_totalPhysMB, t, sizeof(t));
    StrCat2(t, sizeof(t), t, " MB"); WL("Total RAM:    ", t);
    WL("Config:       ", s_is128MB ? "128MB (4x32MB)" : "64MB (4x16MB)");

    IntToStr(s_passCount, t, sizeof(t)); WL("Chunks PASS:  ", t);
    IntToStr(s_failCount, t, sizeof(t)); WL("Chunks FAIL:  ", t);
    IntToStr((int)s_totalErrors, t, sizeof(t)); WL("Total errors: ", t);

    // Per-bank detail
    for (int b = 0; b < NUM_BANKS; ++b)
    {
        char bankLine[64];
        char bIdx[4]; IntToStr(b, bIdx, sizeof(bIdx));
        StrCopy(bankLine, sizeof(bankLine), "Bank ");
        StrCat2(bankLine, sizeof(bankLine), bankLine, bIdx);
        StrCat2(bankLine, sizeof(bankLine), bankLine, ":        ");

        bool anyFail = false;
        for (int c = 0; c < s_chunksPerBank; ++c)
            if (s_chunks[b][c].state == CHUNK_FAIL) anyFail = true;

        StrCat2(bankLine, sizeof(bankLine), bankLine,
            anyFail ? "FAIL" : "PASS");
        StrCat2(bankLine, sizeof(bankLine), bankLine, "\r\n");
        WriteFile(hReport, bankLine, StrLen(bankLine), &w, NULL);
    }

    WL("Result:       ", s_failCount == 0 ? "PASS" : "FAIL - errors detected");
}

// Accessor for XbSet stress loop
int RamStress_GetFailCount() { return s_failCount; }