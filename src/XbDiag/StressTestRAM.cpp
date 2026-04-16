// StressTestRAM.cpp
// XbDiag - RAM stress engine.
//
// Implements the moving inversions soak test across all 4 RAM banks,
// with 11 phases covering write, read+write, read/verify, checkerboard,
// and stride-31 patterns. State machine is independent of the CPU engine.
// Reads s_card, s_ramState, s_ramAbortHold from StressTest.h externs.

#include "StressTestRAM.h"
#include "StressTest.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

#define RAM_CHUNK_SIZE          (2  * 1024 * 1024)
#define RAM_BANK_SIZE_STD       (16 * 1024 * 1024)
#define RAM_BANK_SIZE_EXT       (32 * 1024 * 1024)
#define RAM_NUM_BANKS           4
#define RAM_CHUNKS_PER_BANK_STD 8
#define RAM_CHUNKS_PER_BANK_EXT 16
#define RAM_MAX_CHUNKS          16
#define RAM_PATTERN_A           0xAA55AA55UL
#define RAM_PATTERN_B           0x55AA55AAUL
#define RAM_TICK_MS             30    // time slice per tick — 30ms keeps UI responsive

enum RamChunkState { RCHUNK_UNTESTED = 0, RCHUNK_SKIPPED, RCHUNK_TESTING, RCHUNK_PASS, RCHUNK_FAIL };

enum RamPhase
{
    RPHASE_ALLOC = 0,
    RPHASE_P1_WRITE,
    RPHASE_P2_READW,
    RPHASE_P3_READW,
    RPHASE_P4_READ,
    RPHASE_P5_WRITE,
    RPHASE_P6_READ,
    // Stress extensions
    RPHASE_P7_CHECKER_W,   // fwd write checkerboard 0xAAAAAAAA/0x55555555
    RPHASE_P8_CHECKER_INV, // bwd verify checker, write inverted
    RPHASE_P9_CHECKER_V,   // fwd verify inverted checker
    RPHASE_P10_STRIDE_W,   // stride-31 write addr^0xBAADF00D
    RPHASE_P11_STRIDE_R,   // stride-31 verify
    RPHASE_FREE,
};

struct RamChunk { RamChunkState state; DWORD errorCount; };
static RamChunk    sr_chunks[RAM_NUM_BANKS][RAM_MAX_CHUNKS];
static int         sr_chunksPerBank = RAM_CHUNKS_PER_BANK_STD;
static bool        sr_is128MB = false;

// Counters
static DWORD       sr_totalPhysMB = 0;
static DWORD       sr_availPhysMB = 0;
static DWORD       sr_usedMB = 0;
static int         sr_testedCount = 0;
static int         sr_passCount = 0;
static int         sr_failCount = 0;
static int         sr_skipCount = 0;
static DWORD       sr_totalErrors = 0;

// Sweep / timing
static int         sr_sweep = 0;
static DWORD       sr_sweepErrors = 0;
static DWORD       sr_startTick = 0;

// Per-bank stress state
static int         sr_bank = 0;
static RamPhase    sr_phase = RPHASE_ALLOC;
static DWORD* sr_base = NULL;
static DWORD       sr_dwords = 0;
static DWORD       sr_offset = 0;
static DWORD       sr_bankErr = 0;

// RAM card state machine — independent of CPU s_state
StressState s_ramState = SSTATE_IDLE;
bool        s_ramAbortHold = false;
DWORD       s_ramAbortStart = 0;

extern "C" PVOID __stdcall MmAllocateContiguousMemory(ULONG NumberOfBytes);
extern "C" VOID  __stdcall MmFreeContiguousMemory(PVOID BaseAddress);

static __forceinline void RamFlushCache()
{
    __asm { wbinvd }
}

// ============================================================================
// RAM engine helpers
static void RamResetCounters()
{
    for (int b = 0; b < RAM_NUM_BANKS; ++b)
        for (int c = 0; c < RAM_MAX_CHUNKS; ++c)
        {
            sr_chunks[b][c].state = RCHUNK_UNTESTED; sr_chunks[b][c].errorCount = 0;
        }
    sr_testedCount = 0;
    sr_passCount = 0;
    sr_failCount = 0;
    sr_skipCount = 0;
    sr_totalErrors = 0;
}

static void RamOnStart()
{
    MEMORYSTATUS ms; ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    sr_totalPhysMB = (DWORD)(ms.dwTotalPhys / (1024 * 1024));
    sr_availPhysMB = (DWORD)(ms.dwAvailPhys / (1024 * 1024));
    sr_usedMB = sr_totalPhysMB - sr_availPhysMB;
    sr_is128MB = (sr_totalPhysMB >= 100);
    sr_chunksPerBank = sr_is128MB ? RAM_CHUNKS_PER_BANK_EXT : RAM_CHUNKS_PER_BANK_STD;

    if (sr_base) { MmFreeContiguousMemory(sr_base); sr_base = NULL; }
    RamResetCounters();
    sr_bank = 0;
    sr_phase = RPHASE_ALLOC;
    sr_offset = 0;
    sr_bankErr = 0;
    sr_sweep = 0;
    sr_sweepErrors = 0;
    sr_startTick = GetTickCount();
}

static void RamStop()
{
    if (sr_base) { MmFreeContiguousMemory(sr_base); sr_base = NULL; }
}

static const char* RamPhaseLabel(RamPhase ph)
{
    switch (ph)
    {
    case RPHASE_ALLOC:           return "ALLOCATING";
    case RPHASE_P1_WRITE:        return " 1/11  WRITE    fwd  0xAA55AA55";
    case RPHASE_P2_READW:        return " 2/11  RD+WR    fwd verify/inv";
    case RPHASE_P3_READW:        return " 3/11  RD+WR    bwd verify/fwd";
    case RPHASE_P4_READ:         return " 4/11  READ     fwd 0xAA55AA55";
    case RPHASE_P5_WRITE:        return " 5/11  WRITE    addr^DEADBEEF";
    case RPHASE_P6_READ:         return " 6/11  READ     addr^XOR verify";
    case RPHASE_P7_CHECKER_W:    return " 7/11  WRITE    checker AA/55";
    case RPHASE_P8_CHECKER_INV:  return " 8/11  RD+WR    bwd invert";
    case RPHASE_P9_CHECKER_V:    return " 9/11  READ     checker verify";
    case RPHASE_P10_STRIDE_W:    return "10/11  WRITE    stride-31";
    case RPHASE_P11_STRIDE_R:    return "11/11  READ     stride-31 verify";
    case RPHASE_FREE:            return "COMMITTING";
    default:                     return "";
    }
}

static DWORD RamPhaseColor(RamPhase ph)
{
    switch (ph)
    {
    case RPHASE_P1_WRITE:
    case RPHASE_P5_WRITE:
    case RPHASE_P7_CHECKER_W:
    case RPHASE_P10_STRIDE_W:  return D3DCOLOR_XRGB(220, 120, 20);
    case RPHASE_P2_READW:
    case RPHASE_P3_READW:
    case RPHASE_P8_CHECKER_INV: return D3DCOLOR_XRGB(20, 180, 220);
    case RPHASE_P4_READ:
    case RPHASE_P6_READ:
    case RPHASE_P9_CHECKER_V:
    case RPHASE_P11_STRIDE_R:  return D3DCOLOR_XRGB(40, 200, 80);
    default:              return D3DCOLOR_XRGB(60, 60, 60);
    }
}

static float RamPhaseProgress()
{
    if (sr_dwords == 0) return 0.f;
    if ((sr_phase == RPHASE_P3_READW || sr_phase == RPHASE_P8_CHECKER_INV)
        && sr_offset < sr_dwords)
    {
        DWORD done = sr_dwords - 1 - sr_offset;
        return (float)done / (float)sr_dwords;
    }
    return (float)sr_offset / (float)sr_dwords;
}

static DWORD RamChunkColor(RamChunkState st, bool flash)
{
    switch (st)
    {
    case RCHUNK_PASS:    return COL_GREEN;
    case RCHUNK_FAIL:    return COL_RED;
    case RCHUNK_SKIPPED: return COL_GRAY;
    case RCHUNK_TESTING: return flash ? COL_YELLOW : D3DCOLOR_XRGB(200, 180, 0);
    default:             return COL_DIM;
    }
}

// ============================================================================
// RamStressStep  — time-sliced, call once per tick
// ============================================================================

static void RamStressStep()
{
    DWORD chunkDw = RAM_CHUNK_SIZE / sizeof(DWORD);

    // ---- ALLOC -------------------------------------------------------------
    if (sr_phase == RPHASE_ALLOC)
    {
        for (int c = 0; c < sr_chunksPerBank; ++c)
            sr_chunks[sr_bank][c].state = RCHUNK_TESTING;

        ULONG bankBytes = (ULONG)(sr_is128MB ? RAM_BANK_SIZE_EXT : RAM_BANK_SIZE_STD);
        ULONG tryBytes = bankBytes;
        sr_base = NULL;
        while (!sr_base && tryBytes >= (ULONG)RAM_CHUNK_SIZE)
        {
            sr_base = (DWORD*)MmAllocateContiguousMemory(tryBytes);
            if (!sr_base) tryBytes /= 2;
        }
        if (!sr_base)
        {
            for (int c = 0; c < sr_chunksPerBank; ++c)
            {
                sr_chunks[sr_bank][c].state = RCHUNK_SKIPPED; sr_skipCount++;
            }
            sr_phase = RPHASE_FREE;
            return;
        }
        sr_dwords = tryBytes / sizeof(DWORD);
        sr_offset = 0;
        sr_bankErr = 0;
        sr_phase = RPHASE_P1_WRITE;
        return;
    }

    // ---- FREE / commit -----------------------------------------------------
    if (sr_phase == RPHASE_FREE)
    {
        if (sr_base) { MmFreeContiguousMemory(sr_base); sr_base = NULL; }

        for (int c = 0; c < sr_chunksPerBank; ++c)
        {
            if (sr_chunks[sr_bank][c].state == RCHUNK_TESTING)
            {
                bool anyErr = (sr_chunks[sr_bank][c].errorCount > 0);
                sr_chunks[sr_bank][c].state = anyErr ? RCHUNK_FAIL : RCHUNK_PASS;
                sr_testedCount++;
                if (anyErr) sr_failCount++;
                else        sr_passCount++;
            }
        }
        sr_totalErrors += sr_bankErr;
        sr_bank++;

        if (sr_bank >= RAM_NUM_BANKS)
        {
            // Completed one sweep — reset grid and go again
            sr_sweep++;
            sr_sweepErrors += sr_totalErrors;
            RamResetCounters();
            sr_bank = 0;
        }
        sr_phase = RPHASE_ALLOC;
        sr_offset = 0;
        return;
    }

    // ---- Active pass — time-sliced -----------------------------------------
    DWORD tickStart = GetTickCount();
    DWORD* p = sr_base;
    DWORD  tot = sr_dwords;
    DWORD  off = sr_offset;

    if (sr_phase == RPHASE_P1_WRITE)
    {
        while (off < tot)
        {
            p[off] = RAM_PATTERN_A; off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off >= tot) { RamFlushCache(); sr_phase = RPHASE_P2_READW; sr_offset = 0; return; }
    }
    else if (sr_phase == RPHASE_P2_READW)
    {
        while (off < tot)
        {
            if (p[off] != RAM_PATTERN_A)
            {
                int c = (int)(off / chunkDw); if (c >= sr_chunksPerBank) c = sr_chunksPerBank - 1;
                sr_chunks[sr_bank][c].errorCount++; sr_bankErr++;
            }
            p[off] = RAM_PATTERN_B; off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off >= tot) { RamFlushCache(); sr_phase = RPHASE_P3_READW; sr_offset = tot - 1; return; }
    }
    else if (sr_phase == RPHASE_P3_READW)
    {
        while (off < tot)
        {
            if (p[off] != RAM_PATTERN_B)
            {
                int c = (int)(off / chunkDw); if (c >= sr_chunksPerBank) c = sr_chunksPerBank - 1;
                sr_chunks[sr_bank][c].errorCount++; sr_bankErr++;
            }
            p[off] = RAM_PATTERN_A;
            if (off == 0) { off = 0xFFFFFFFFUL; break; }
            off--;
            if ((off & 0x3FF) == 0x3FF && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off == 0xFFFFFFFFUL) { RamFlushCache(); sr_phase = RPHASE_P4_READ; sr_offset = 0; return; }
    }
    else if (sr_phase == RPHASE_P4_READ)
    {
        while (off < tot)
        {
            if (p[off] != RAM_PATTERN_A)
            {
                int c = (int)(off / chunkDw); if (c >= sr_chunksPerBank) c = sr_chunksPerBank - 1;
                sr_chunks[sr_bank][c].errorCount++; sr_bankErr++;
            }
            off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off >= tot) { sr_phase = RPHASE_P5_WRITE; sr_offset = 0; return; }
    }
    else if (sr_phase == RPHASE_P5_WRITE)
    {
        while (off < tot)
        {
            p[off] = off ^ 0xDEADBEEFUL; off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off >= tot) { RamFlushCache(); sr_phase = RPHASE_P6_READ; sr_offset = 0; return; }
    }
    else if (sr_phase == RPHASE_P6_READ)
    {
        while (off < tot)
        {
            if (p[off] != (off ^ 0xDEADBEEFUL))
            {
                int c = (int)(off / chunkDw); if (c >= sr_chunksPerBank) c = sr_chunksPerBank - 1;
                sr_chunks[sr_bank][c].errorCount++; sr_bankErr++;
            }
            off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off >= tot) { RamFlushCache(); sr_phase = RPHASE_P7_CHECKER_W; sr_offset = 0; return; }
    }
    // ---- P7: checkerboard write (fwd) -----------------------------------
    // Alternating 0xAAAAAAAA / 0x55555555 per dword.
    // Creates maximum bit-switching pressure between adjacent cells.
    else if (sr_phase == RPHASE_P7_CHECKER_W)
    {
        while (off < tot)
        {
            p[off] = (off & 1) ? 0x55555555UL : 0xAAAAAAAAUL; off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off >= tot) { RamFlushCache(); sr_phase = RPHASE_P8_CHECKER_INV; sr_offset = tot - 1; return; }
    }
    // ---- P8: checkerboard invert (bwd read+verify, write inverted) ------
    // Backward walk: verify AA/55, write 55/AA.
    // Backward direction forces different row-to-row transitions than P7.
    else if (sr_phase == RPHASE_P8_CHECKER_INV)
    {
        while (off < tot)
        {
            DWORD expect = (off & 1) ? 0x55555555UL : 0xAAAAAAAAUL;
            if (p[off] != expect)
            {
                int c = (int)(off / chunkDw); if (c >= sr_chunksPerBank) c = sr_chunksPerBank - 1;
                sr_chunks[sr_bank][c].errorCount++; sr_bankErr++;
            }
            p[off] = (off & 1) ? 0xAAAAAAAAUL : 0x55555555UL;  // inverted
            if (off == 0) { off = 0xFFFFFFFFUL; break; }
            off--;
            if ((off & 0x3FF) == 0x3FF && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off == 0xFFFFFFFFUL) { RamFlushCache(); sr_phase = RPHASE_P9_CHECKER_V; sr_offset = 0; return; }
    }
    // ---- P9: checkerboard verify (fwd) ----------------------------------
    // Verify inverted pattern placed by P8.
    else if (sr_phase == RPHASE_P9_CHECKER_V)
    {
        while (off < tot)
        {
            DWORD expect = (off & 1) ? 0xAAAAAAAAUL : 0x55555555UL;
            if (p[off] != expect)
            {
                int c = (int)(off / chunkDw); if (c >= sr_chunksPerBank) c = sr_chunksPerBank - 1;
                sr_chunks[sr_bank][c].errorCount++; sr_bankErr++;
            }
            off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off >= tot) { sr_phase = RPHASE_P10_STRIDE_W; sr_offset = 0; return; }
    }
    // ---- P10: stride-31 write -------------------------------------------
    // Visits every dword in a non-sequential order using stride 31.
    // 31 is prime and odd, so gcd(31, 2^k) = 1 — complete coverage guaranteed
    // for any power-of-2 buffer.  Writes addr ^ 0xBAADF00D at each location.
    // Cache-unfriendly jumps stress the memory controller and prefetch logic.
    else if (sr_phase == RPHASE_P10_STRIDE_W)
    {
        while (off < tot)
        {
            DWORD actual = (DWORD)(((DWORD64)off * 31UL) % tot);
            p[actual] = actual ^ 0xBAADF00DUL; off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off >= tot) { RamFlushCache(); sr_phase = RPHASE_P11_STRIDE_R; sr_offset = 0; return; }
    }
    // ---- P11: stride-31 verify ------------------------------------------
    // Same stride walk as P10 — verifies addr ^ 0xBAADF00D at each location.
    else if (sr_phase == RPHASE_P11_STRIDE_R)
    {
        while (off < tot)
        {
            DWORD actual = (DWORD)(((DWORD64)off * 31UL) % tot);
            if (p[actual] != (actual ^ 0xBAADF00DUL))
            {
                int c = (int)(actual / chunkDw); if (c >= sr_chunksPerBank) c = sr_chunksPerBank - 1;
                sr_chunks[sr_bank][c].errorCount++; sr_bankErr++;
            }
            off++;
            if ((off & 0x3FF) == 0 && (GetTickCount() - tickStart) >= RAM_TICK_MS) break;
        }
        if (off >= tot) { sr_phase = RPHASE_FREE; sr_offset = 0; return; }
    }

    sr_offset = off;
}

// ============================================================================
// RenderRAMCard
// ============================================================================
//
// Layout:
//   Tab strip                                     Y=TAB_Y..TAB_Y+TAB_H
//   COL_SPLIT = 300 — vertical divider
//   Left (x=LM..COL_SPLIT):
//     Memory map block (config/total/avail/used)
//     Bank address table (B0-B3)
//     Status rows: phase label + phase bar + sweep/errors/elapsed
//   Right (x=GRID_LM..608):
//     4-row chunk grid (identical to RamTest)
//     Lower: IDLE prompt / CONFIRM overlay / RUNNING abort bar
//
// The CONFIRM modal and RUNNING abort-hold bar live inside the card render
// so they overlay the grid naturally, same as the CPU card's overlays.
// ============================================================================

static void RenderRAMCard(const DiagLogo& logo)
{
    const char* hints;
    if (s_ramState == SSTATE_IDLE)
        hints = "[Left] CPU    [A] Start    [B] Back";
    else if (s_ramState == SSTATE_CONFIRM)
        hints = "Hold LT+RT to confirm    [B] Cancel";
    else
        hints = "[Left] CPU    Hold Back+A 5s to Abort";

    DrawPageChrome(logo, "STRESS TEST  -  RAM", hints);
    ST_DrawTabStrip(CARD_RAM);

    // ---- Shared layout constants (mirror RamTest's Render exactly) ---------
    const float COL_SPLIT = 310.f;
    const float MAP_LM = LM;
    const float GRID_LM = COL_SPLIT + 24.f;
    const float TS = 1.3f;
    const float LH = LINE_H - 2.f;
    const float SLH = LINE_H - 4.f;
    const float VM_ = MAP_LM + 90.f;    // value column for left panel

    float y = ST_PANEL_TOP;    // start just below tab strip

    // ---- Section headers ---------------------------------------------------
    DrawText(MAP_LM, y, "MEMORY MAP", TS, COL_YELLOW);
    DrawText(GRID_LM, y, "PHYSICAL BANK MAP", TS, COL_YELLOW);
    y += LH + 2.f;
    HLine(y, MAP_LM, COL_SPLIT - 8.f, COL_BORDER);
    HLine(y, GRID_LM, SW - LM, COL_BORDER);
    y += 5.f;

    // ---- Left: RAM summary -------------------------------------------------
    char buf[48];

    DrawText(MAP_LM, y, "CONFIG  :", TS, COL_GRAY);
    DrawText(VM_, y, sr_is128MB ? "128MB" : "64MB", TS, COL_CYAN);
    y += LH;
    DrawText(VM_, y, sr_is128MB ? "4x32MB  dual rank" : "4x16MB  single rank", TS, COL_CYAN);
    y += LH;

    IntToStr((int)sr_totalPhysMB, buf, sizeof(buf));
    StrCat2(buf, sizeof(buf), buf, "MB");
    DrawText(MAP_LM, y, "TOTAL   :", TS, COL_GRAY);
    DrawText(VM_, y, buf, TS, COL_WHITE);
    y += LH;

    IntToStr((int)sr_availPhysMB, buf, sizeof(buf));
    StrCat2(buf, sizeof(buf), buf, "MB");
    DrawText(MAP_LM, y, "AVAIL   :", TS, COL_GRAY);
    DrawText(VM_, y, buf, TS, COL_WHITE);
    y += LH;

    IntToStr((int)sr_usedMB, buf, sizeof(buf));
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

    DWORD bankSizeMB = sr_is128MB ? 32 : 16;
    for (int b = 0; b < RAM_NUM_BANKS; ++b)
    {
        bool active = (s_ramState == SSTATE_RUNNING && sr_bank == b);
        DWORD rowCol = active ? COL_YELLOW : COL_WHITE;

        char bankBuf[4];
        IntToStr(b, bankBuf, sizeof(bankBuf));
        DrawText(MAP_LM, y, bankBuf, TS, active ? COL_CYAN : COL_GRAY);

        char addrHex[12], addrFull[14];
        IntToHex((DWORD)(b * (sr_is128MB ? RAM_BANK_SIZE_EXT : RAM_BANK_SIZE_STD)), 8, addrHex, sizeof(addrHex));
        StrCat2(addrFull, sizeof(addrFull), "0x", addrHex);
        DrawText(MAP_LM + 36.f, y, addrFull, TS, COL_CYAN);

        char szBuf[8];
        IntToStr((int)bankSizeMB, szBuf, sizeof(szBuf));
        StrCat2(szBuf, sizeof(szBuf), szBuf, "MB");
        DrawText(MAP_LM + 122.f, y, szBuf, TS, rowCol);
        DrawText(MAP_LM + 162.f, y, sr_is128MB ? "2" : "1", TS, rowCol);
        y += LH;
    }

    y += 4.f;
    HLine(y, MAP_LM, COL_SPLIT - 8.f, COL_BORDER);
    y += 5.f;

    // ---- Left lower: status block ------------------------------------------
    if (s_ramState == SSTATE_IDLE)
    {
        DrawText(MAP_LM, y, "[A] Start RAM Stress", TS, COL_YELLOW);
    }
    else
    {
        // Overall bank progress bar
        if (s_ramState == SSTATE_RUNNING)
        {
            int totalChunks = RAM_NUM_BANKS * sr_chunksPerBank;
            int doneChunks = sr_bank * sr_chunksPerBank;
            const float BAR_W = COL_SPLIT - MAP_LM - 8.f;
            const float BAR_H = 8.f;
            float fillW = (totalChunks > 0) ? BAR_W * ((float)doneChunks / (float)totalChunks) : 0.f;
            FillRect(MAP_LM, y, MAP_LM + BAR_W, y + BAR_H, D3DCOLOR_XRGB(20, 25, 55));
            if (fillW > 0.f)
                FillRectGrad(MAP_LM, y, MAP_LM + fillW, y + BAR_H,
                    D3DCOLOR_XRGB(60, 160, 255), D3DCOLOR_XRGB(30, 90, 180));
            HLine(y, MAP_LM, MAP_LM + BAR_W, COL_BORDER);
            HLine(y + BAR_H, MAP_LM, MAP_LM + BAR_W, COL_BORDER);
            VLine(MAP_LM, y, y + BAR_H, COL_BORDER);
            VLine(MAP_LM + BAR_W, y, y + BAR_H, COL_BORDER);

            char cb[4], tb[4];
            IntToStr(sr_bank + 1, cb, sizeof(cb));
            IntToStr(RAM_NUM_BANKS, tb, sizeof(tb));
            char progBuf[24];
            StrCat3(progBuf, sizeof(progBuf), "Bank ", cb, " of ");
            StrCat2(progBuf, sizeof(progBuf), progBuf, tb);
            DrawText(MAP_LM, y + BAR_H + 2.f, progBuf, TS, COL_YELLOW);
            y += BAR_H + SLH + 2.f;

            // Phase label
            DrawText(MAP_LM, y, "PHASE   :", TS, COL_GRAY);
            DrawText(MAP_LM + TW("PHASE   :", TS) + 4.f, y,
                RamPhaseLabel(sr_phase), 1.05f, COL_CYAN);
            y += SLH;

            // Phase progress bar
            float subProg = RamPhaseProgress();
            const float BAR_W2 = COL_SPLIT - MAP_LM - 8.f;
            const float BAR_H2 = 8.f;
            float fillW2 = BAR_W2 * subProg;
            DWORD barCol = RamPhaseColor(sr_phase);
            FillRect(MAP_LM, y, MAP_LM + BAR_W2, y + BAR_H2, D3DCOLOR_XRGB(15, 18, 40));
            float dw2 = fillW2 > 2.f ? fillW2 : 2.f;
            FillRect(MAP_LM, y, MAP_LM + dw2, y + BAR_H2, barCol);
            HLine(y, MAP_LM, MAP_LM + BAR_W2, COL_BORDER);
            HLine(y + BAR_H2, MAP_LM, MAP_LM + BAR_W2, COL_BORDER);
            VLine(MAP_LM, y, y + BAR_H2, COL_BORDER);
            VLine(MAP_LM + BAR_W2, y, y + BAR_H2, COL_BORDER);
            int pct = Ftoi(subProg * 100.f);
            char pctBuf[8];
            IntToStr(pct, pctBuf, sizeof(pctBuf));
            StrCat2(pctBuf, sizeof(pctBuf), pctBuf, "%");
            DrawText(MAP_LM + BAR_W2 - 28.f, y, pctBuf, 1.0f, D3DCOLOR_XRGB(220, 220, 220));
            y += BAR_H2 + 3.f;
        }

        // Elapsed / sweeps / error counts
        if (s_ramState == SSTATE_RUNNING || s_ramState == SSTATE_CONFIRM)
        {
            DWORD elSec = (GetTickCount() - sr_startTick) / 1000;
            char mm[4], ss[4];
            IntToStr((int)(elSec / 60), mm, sizeof(mm));
            IntToStr((int)(elSec % 60), ss, sizeof(ss));
            char elBuf[16];
            elBuf[0] = 0;
            StrCat2(elBuf, sizeof(elBuf), elBuf, mm);
            StrCat2(elBuf, sizeof(elBuf), elBuf, "m ");
            StrCat2(elBuf, sizeof(elBuf), elBuf, ss);
            StrCat2(elBuf, sizeof(elBuf), elBuf, "s");
            DrawText(MAP_LM, y, "ELAPSED :", TS, COL_GRAY);
            DrawText(VM_, y, elBuf, TS, COL_WHITE);
            y += SLH;

            char swBuf[8];
            IntToStr(sr_sweep, swBuf, sizeof(swBuf));
            DrawText(MAP_LM, y, "SWEEPS  :", TS, COL_GRAY);
            DrawText(VM_, y, swBuf, TS, COL_WHITE);
            y += SLH;

            DWORD totalErr = sr_sweepErrors + sr_totalErrors;
            UIntToStr(totalErr, buf, sizeof(buf));
            DrawText(MAP_LM, y, "ERRORS  :", TS, COL_GRAY);
            DrawText(VM_, y, buf, TS, totalErr > 0 ? COL_RED : COL_GREEN);
            y += SLH;

            IntToStr(sr_passCount, buf, sizeof(buf));
            DrawText(MAP_LM, y, "PASSED  :", TS, COL_GRAY);
            DrawText(VM_, y, buf, TS, COL_GREEN);
            y += SLH;

            IntToStr(sr_failCount, buf, sizeof(buf));
            DrawText(MAP_LM, y, "FAILED  :", TS, COL_GRAY);
            DrawText(VM_, y, buf, TS, sr_failCount > 0 ? COL_RED : COL_DIM);
            y += SLH;
        }
    }

    // ---- Vertical divider --------------------------------------------------
    VLine(COL_SPLIT, ST_PANEL_TOP + LH + 7.f, BOT_BAR_Y - 4.f, COL_BORDER);

    // ---- Right: chunk grid (identical to RamTest's grid block) -------------
    {
        const float GRID_RIGHT = SW - 8.f;
        const float CELL_W = (GRID_RIGHT - GRID_LM) / (float)RAM_MAX_CHUNKS - 2.f;
        const float CELL_H = 38.f;
        const float CELL_PAD = 2.f;
        const float ROW_PAD = 8.f;
        bool flash = ((GetTickCount() / 200) & 1) != 0;
        float gridY = ST_PANEL_TOP + LH + 7.f;

        for (int b = 0; b < RAM_NUM_BANKS; ++b)
        {
            float rowY = gridY + (float)b * (CELL_H + ROW_PAD);
            bool bankActive = (s_ramState == SSTATE_RUNNING && sr_bank == b);

            char bankLbl[4], bankRow[8];
            IntToStr(b, bankLbl, sizeof(bankLbl));
            StrCat2(bankRow, sizeof(bankRow), "B", bankLbl);
            DrawText(GRID_LM - 20.f, rowY + 10.f, bankRow, 1.2f,
                bankActive ? COL_YELLOW : COL_GRAY);

            for (int c = 0; c < sr_chunksPerBank; ++c)
            {
                float cx = GRID_LM + (float)c * (CELL_W + CELL_PAD);
                DWORD cellCol = RamChunkColor(sr_chunks[b][c].state, bankActive && flash);
                FillRect(cx, rowY, cx + CELL_W, rowY + CELL_H, cellCol);

                char mbBuf[8];
                if (sr_is128MB)
                    IntToStr(c, mbBuf, sizeof(mbBuf));
                else
                {
                    int mbStart = b * 16 + c * 2;
                    IntToStr(mbStart, mbBuf, sizeof(mbBuf));
                }
                DWORD lblCol = (sr_chunks[b][c].state == RCHUNK_PASS ||
                    sr_chunks[b][c].state == RCHUNK_SKIPPED)
                    ? COL_BG : D3DCOLOR_XRGB(40, 40, 40);
                DrawText(cx + 2.f, rowY + 4.f, mbBuf, 1.0f, lblCol);

                if (sr_chunks[b][c].state == RCHUNK_FAIL)
                {
                    char eBuf[8];
                    UIntToStr(sr_chunks[b][c].errorCount, eBuf, sizeof(eBuf));
                    DrawText(cx + 2.f, rowY + CELL_H - 12.f, eBuf, 1.0f,
                        D3DCOLOR_XRGB(255, 200, 200));
                }
            }

            // Dim unused columns for 64MB
            if (!sr_is128MB)
            {
                for (int c = RAM_CHUNKS_PER_BANK_STD; c < RAM_MAX_CHUNKS; ++c)
                {
                    float cx = GRID_LM + (float)c * (CELL_W + CELL_PAD);
                    FillRect(cx, rowY, cx + CELL_W, rowY + CELL_H, D3DCOLOR_XRGB(12, 14, 28));
                    HLine(rowY + CELL_H * 0.5f - 1.f, cx + 3.f, cx + CELL_W - 3.f, D3DCOLOR_XRGB(45, 45, 60));
                    VLine(cx + CELL_W * 0.5f, rowY + 3.f, rowY + CELL_H - 3.f, D3DCOLOR_XRGB(45, 45, 60));
                }
            }
        }

        // ---- Lower block: phase key + status / confirm / abort -------------
        float lowerY = gridY + (float)RAM_NUM_BANKS * (CELL_H + ROW_PAD) + 4.f;

        if (s_ramState == SSTATE_IDLE)
        {
            DrawText(GRID_LM, lowerY, "[A] Start    Runs continuously until you abort.", 1.15f, COL_YELLOW);
            lowerY += LINE_H;
            DrawText(GRID_LM, lowerY,
                sr_is128MB
                ? "128MB: chunks 0-7=CHIP1  8-15=CHIP2"
                : "64MB: any fail = that bank's chip suspect",
                1.1f, COL_CYAN);
        }
        else if (s_ramState == SSTATE_CONFIRM)
        {
            // Confirm overlay box
            const float OW = GRID_RIGHT - GRID_LM;
            const float OH = 56.f;
            FillRect(GRID_LM, lowerY, GRID_LM + OW, lowerY + OH, D3DCOLOR_ARGB(200, 10, 14, 40));
            HLine(lowerY, GRID_LM, GRID_LM + OW, COL_CYAN);
            HLine(lowerY + OH, GRID_LM, GRID_LM + OW, COL_CYAN);
            VLine(GRID_LM, lowerY, lowerY + OH, COL_CYAN);
            VLine(GRID_LM + OW, lowerY, lowerY + OH, COL_CYAN);

            DrawText(GRID_LM + 8.f, lowerY + 6.f,
                "RAM STRESS TEST — CONFIRM START", 1.25f, COL_YELLOW);
            DrawText(GRID_LM + 8.f, lowerY + 22.f,
                "Runs continuously. Hold LT + RT to begin.", 1.15f, COL_WHITE);
            DrawText(GRID_LM + 8.f, lowerY + 36.f,
                "[B] Cancel", 1.1f, COL_GRAY);
        }
        else // RUNNING
        {
            // Phase colour key
            HLine(lowerY, GRID_LM, GRID_RIGHT, COL_BORDER);
            lowerY += 4.f;

            FillRect(GRID_LM, lowerY + 3.f, GRID_LM + 10.f, lowerY + 11.f, D3DCOLOR_XRGB(220, 120, 20));
            DrawText(GRID_LM + 13.f, lowerY, "WRITE", 1.1f, COL_DIM);
            FillRect(GRID_LM + 65.f, lowerY + 3.f, GRID_LM + 75.f, lowerY + 11.f, D3DCOLOR_XRGB(20, 180, 220));
            DrawText(GRID_LM + 78.f, lowerY, "READ+WRITE", 1.1f, COL_DIM);
            FillRect(GRID_LM + 175.f, lowerY + 3.f, GRID_LM + 185.f, lowerY + 11.f, D3DCOLOR_XRGB(40, 200, 80));
            DrawText(GRID_LM + 188.f, lowerY, "READ/VERIFY", 1.1f, COL_DIM);
            lowerY += LINE_H;

            // Abort hold bar — same UX as CPU card
            if (s_ramAbortHold)
            {
                DWORD held = GetTickCount() - s_ramAbortStart;
                float abortFrac = held >= ABORT_HOLD_MS ? 1.f : (float)held / ABORT_HOLD_MS;
                const float ABW = GRID_RIGHT - GRID_LM;
                FillRect(GRID_LM, lowerY, GRID_LM + ABW, lowerY + 8.f, D3DCOLOR_XRGB(15, 10, 10));
                FillRect(GRID_LM, lowerY, GRID_LM + ABW * abortFrac, lowerY + 8.f, D3DCOLOR_XRGB(200, 30, 30));
                HLine(lowerY, GRID_LM, GRID_LM + ABW, COL_BORDER);
                HLine(lowerY + 8.f, GRID_LM, GRID_LM + ABW, COL_BORDER);
                lowerY += 10.f;

                int remSec = (int)((ABORT_HOLD_MS - (held < ABORT_HOLD_MS ? held : ABORT_HOLD_MS)) / 1000) + 1;
                char abortMsg[48];
                char secBuf[4];
                IntToStr(remSec, secBuf, sizeof(secBuf));
                abortMsg[0] = 0;
                StrCat2(abortMsg, sizeof(abortMsg), abortMsg, "ABORTING IN ");
                StrCat2(abortMsg, sizeof(abortMsg), abortMsg, secBuf);
                StrCat2(abortMsg, sizeof(abortMsg), abortMsg, "s  --  RELEASE TO CANCEL");
                DrawText(GRID_LM, lowerY, abortMsg, 1.15f, COL_RED);
            }
            else
            {
                DrawText(GRID_LM, lowerY, "Hold Back+A for 5s to abort.", 1.1f, COL_DIM);
            }
        }
    }
}


// ============================================================================
// RamStress_AutoRun — timed RAM stress using the StressTest RAM engine
// Calls RamOnStart() then drives RamStressStep() until durationMs expires,
// then calls RamStop() and writes results to hReport.
// ============================================================================

void RamStress_AutoRun(HANDLE hReport, DWORD durationMs)
{
    // Full reset of RAM stress state
    StressTest_OnEnter();
    RamOnStart();
    s_ramState = SSTATE_RUNNING;

    DWORD endTime = GetTickCount() + durationMs;

    static DWORD ramNextRender = 0;
    while (GetTickCount() < endTime)
    {
        RamStressStep();

        // Render status every ~500ms
        DWORD nowR2 = GetTickCount();
        if (nowR2 >= ramNextRender && g_pDevice)
        {
            DWORD remain2 = (nowR2 < endTime) ? (endTime - nowR2) / 1000 : 0;
            g_pDevice->BeginScene();
            DWORD dim2 = D3DCOLOR_XRGB(10, 13, 30);
            g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET, dim2, 1.f, 0);
            float py2 = 40.f;
            DrawText(12.f, py2, "RAM STRESS TEST (AUTO)", 1.4f,
                D3DCOLOR_XRGB(255, 220, 60)); py2 += 24.f;
            char sb2[64]; char rm2[8], sw2[8], er2[8];
            IntToStr((int)remain2, rm2, sizeof(rm2));
            IntToStr(sr_sweep, sw2, sizeof(sw2));
            IntToStr((int)(sr_totalErrors + sr_sweepErrors), er2, sizeof(er2));
            StrCopy(sb2, sizeof(sb2), "Remaining: ");
            StrCat2(sb2, sizeof(sb2), sb2, rm2);
            StrCat2(sb2, sizeof(sb2), sb2, "s   Sweeps: ");
            StrCat2(sb2, sizeof(sb2), sb2, sw2);
            StrCat2(sb2, sizeof(sb2), sb2, "   Errors: ");
            StrCat2(sb2, sizeof(sb2), sb2, er2);
            DrawText(12.f, py2, sb2, 1.2f, D3DCOLOR_XRGB(180, 180, 180));
            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);
            ramNextRender = nowR2 + 500;
        }
    }

    // Abort cleanly
    RamStop();
    s_ramState = SSTATE_IDLE;

    if (!hReport || hReport == INVALID_HANDLE_VALUE) return;

    char line[128]; DWORD w;
    auto WL = [&](const char* lbl, const char* val)
        {
            StrCopy(line, sizeof(line), lbl);
            StrCat2(line, sizeof(line), line, val);
            StrCat2(line, sizeof(line), line, "\r\n");
            WriteFile(hReport, line, StrLen(line), &w, NULL);
        };

    char t[16];
    IntToStr((int)(durationMs / 1000), t, sizeof(t));
    StrCat2(t, sizeof(t), t, "s");             WL("Duration:      ", t);
    IntToStr(sr_sweep, t, sizeof(t));          WL("Sweeps:        ", t);
    IntToStr(sr_passCount, t, sizeof(t));      WL("Chunks Pass:   ", t);
    IntToStr(sr_failCount, t, sizeof(t));      WL("Chunks Fail:   ", t);
    IntToStr((int)(sr_totalErrors + sr_sweepErrors), t, sizeof(t));
    WL("Total Errors:  ", t);
    WL("Result:        ",
        (sr_failCount == 0 && sr_totalErrors == 0) ? "PASS" : "FAIL - errors detected");
}

// ============================================================================
// AutoRun result accessors — for XbSet loop accumulation
// Called by XbSet after each StressTest_AutoRun / RamStress_AutoRun call
// to read the per-loop stats and build cross-loop summaries.
// ============================================================================


DWORD RamAutoRun_GetSweeps() { return (DWORD)sr_sweep; }
DWORD RamAutoRun_GetErrors() { return sr_totalErrors + sr_sweepErrors; }

// ============================================================================
// Public API wrappers
// ============================================================================

void ST_RAM_OnStart() { RamOnStart(); }
void ST_RAM_Stop() { RamStop(); }
void ST_RAM_Step() { RamStressStep(); }

void ST_RAM_Render(const DiagLogo& logo) { RenderRAMCard(logo); }
int   RamAutoRun_GetFailed() { return sr_failCount; }