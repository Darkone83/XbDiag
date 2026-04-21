// StressTestCPU.cpp
// XbDiag - CPU stress engine: sensor sampling, MHz measurement, and CPU card render.
//
// ReadCPUMHz reads MCPX CPUMPLL (PCI 0:3:0 offset 0x6C) + MSR 0x2A ratio.
// TakeSample reads CPU/MB temp and fan via SMBus PIC registers.
// All CPU render functions (DrawInfoPanel, DrawGraph, overlays, RenderCPUCard) live here.

#include "StressTestCPU.h"
#include "StressTest.h"
#include "StressMath.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

// ============================================================================
// Fan override state
// s_fanAuto = true  → SMC controls fan (default)
// s_fanAuto = false → manual target written to PIC reg 0x06
// s_fanTargetPct is in 5% steps (0, 5, 10, ... 100)
// ============================================================================

static bool s_fanAuto = true;
static int  s_fanTargetPct = 0;    // last applied manual target; 0 = min
// Exact value, no SMBus dependency, works on all CPU variants including
// Tualatin upgrades where the ICS clock generator may be absent or replaced.
// CPUMPLL is cached on first call — the PLL config never changes at runtime.
// Matches SysInfo MeasureCpuMHz() exactly so both modules always agree.
// ============================================================================

extern "C" VOID __stdcall HalReadWritePCISpace(
    ULONG BusNumber, ULONG SlotNumber, ULONG RegisterNumber,
    PVOID Buffer, ULONG Length, BOOLEAN WritePCISpace);

// Xbox crystal reference frequency — 16.666... MHz.
// Matches SysInfo XTAL_HZ exactly; both modules must use the same constant.
static const double XTAL_HZ = 16666666.6667;

static int ReadCPUMHz()
{
    // Cache CPUMPLL — only need to read PCI config once
    static DWORD s_cpumpll = 0;
    static bool  s_cpumpllRead = false;
    if (!s_cpumpllRead)
    {
        // SlotNumber: dev[4:0] in bits [4:0], func[2:0] in bits [7:5]
        // PCI_SLOT_NUMBER format: (dev << 5) | func  — dev=3, func=0
        ULONG slot = ((3UL & 0x1F) << 5) | (0UL & 0x07);
        HalReadWritePCISpace(0, slot, 0x6C, &s_cpumpll, sizeof(s_cpumpll), FALSE);
        s_cpumpllRead = true;
    }

    DWORD fsb_div = s_cpumpll & 0xFF;
    DWORD fsb_mult = (s_cpumpll >> 8) & 0xFF;
    if (fsb_div == 0 || fsb_mult == 0) return 733;

    double fsb_hz = XTAL_HZ * ((double)fsb_mult / (double)fsb_div);

    // CPU ratio from MSR 0x2A bits [27:22] masked 0x2F
    DWORD msr_lo = 0;
    __asm
    {
        mov  ecx, 0x2A
        rdmsr
        mov  msr_lo, eax
    }
    BYTE pat = (BYTE)((msr_lo >> 22) & 0x2F);
    DWORD ratio = 0;
    switch (pat)
    {
    case 0x01: ratio = 30;  break;
    case 0x05: ratio = 35;  break;
    case 0x02: ratio = 40;  break;
    case 0x06: ratio = 45;  break;
    case 0x00: ratio = 50;  break;
    case 0x04: ratio = 55;  break;
    case 0x0B: ratio = 60;  break;
    case 0x0F: ratio = 65;  break;
    case 0x09: ratio = 70;  break;
    case 0x0D: ratio = 75;  break;
    case 0x0A: ratio = 80;  break;
    case 0x26: ratio = 85;  break;
    case 0x20: ratio = 90;  break;
    case 0x24: ratio = 95;  break;
    case 0x2B: ratio = 100; break;
    case 0x2F: ratio = 105; break;
    case 0x2A: ratio = 130; break;
    case 0x2C: ratio = 140; break;
    default:   return 733;
    }

    DWORD cpu_mhz = (DWORD)((fsb_hz * ((double)ratio / 10.0)) / 1.0e6 + 0.5);
    if (cpu_mhz < 400 || cpu_mhz > 1600) return 733;
    return (int)cpu_mhz;
}

// ============================================================================
// Sensor sample — reads CPU temp, MB temp, and fan speed from PIC/SMC.
// ============================================================================

static void TakeSample()
{
    // ---- Path detection (one-shot) ----------------------------------------
    // Always read CPU temp from PIC reg 0x09 — the SMC proxies the ADM1032
    // internally on 1.0-1.5, so this works on all revisions without a direct
    // ADM1032 read.  1.6 detection: probe Xcalibur encoder at 0x70 (only
    // present on 1.6 boards).
    if (!s_pathKnown)
    {
        BYTE dummy = 0;
        s_is16 = SMBusRead(0xE0, 0x00, dummy);  // Xcalibur at 0x70<<1=0xE0
        // Only commit once PIC responds — avoids locking wrong is16 if
        // the bus is busy at first sample time.
        BYTE probe = 0;
        if (SMBusRead(SMBADDR_PIC, 0x09, probe))
            s_pathKnown = true;
    }

    // ---- Fan readback (PIC reg 0x10) ---------------------------------------
    // Clear s_fanOK at the start of every sample so a failed read is never
    // masked by a previous success — stale fan values must not appear live.
    // Upper bound guard: > 50 is an invalid PIC PWM value, discard.
    s_fanOK = false;
    {
        BYTE fanRaw = 0;
        if (SMBusRead(SMBADDR_PIC, 0x10, fanRaw) && fanRaw <= 50)
        {
            s_curFan = fanRaw;
            s_fanOK = true;
        }
        else if (SMBusRead(SMBADDR_PIC, 0x06, fanRaw) && fanRaw <= 50)
        {
            s_curFan = fanRaw;
            s_fanOK = true;
        }
    }

    // ---- CPU + MB temp — read both atomically, commit both or neither ------
    // s_curMB is only updated when the MB read succeeds in the same sample as
    // the CPU read.  This prevents a stale MB value from participating in the
    // thermal abort comparison against a freshly read CPU temp (or vice versa).
    // 1.6 MB correction: val*0.8 - 3.556 (matches xbox_smbus_poll.cpp exactly).
    {
        BYTE cpu = 0, mb = 0;
        bool cpuOK = SMBusRead(SMBADDR_PIC, 0x09, cpu);
        bool mbOK = SMBusRead(SMBADDR_PIC, 0x0A, mb);

        s_sensorOK = cpuOK;
        s_mbOK = mbOK;

        if (cpuOK) s_curCPU = cpu;

        if (mbOK)
        {
            if (s_is16)
            {
                int adj = ((int)mb * 4 / 5) - 4;
                s_curMB = (BYTE)(adj < 0 ? 0 : adj);
            }
            else
            {
                s_curMB = mb;
            }
        }

        if (!cpuOK) return;

        // Thermal abort — use max(CPU, MB) to match SMC fan-curve behaviour.
        // Only include MB in the comparison if the MB read was fresh this sample.
        BYTE hotTemp = (s_mbOK && s_curMB > s_curCPU) ? s_curMB : s_curCPU;
        if (s_state == SSTATE_RUNNING && (int)hotTemp >= s_tempThreshold)
        {
            s_state = SSTATE_IDLE;
            s_thermalAbort = true;
            // Release manual fan override — SMC must resume thermal control
            // immediately on abort. Without this the fan stays locked at the
            // manual target while the CPU is already over threshold.
            ST_CPU_FanRelease();
        }

        BYTE load = (s_state == SSTATE_RUNNING) ? s_measuredLoad : 0;
        BYTE fanPct = s_fanOK ? (BYTE)((int)s_curFan * 2) : 0;
        ST_PushHistory(cpu, load, fanPct);
    }
}

// ============================================================================
// DrawInfoPanel — compact panel with large readout + tri-color gauge bar
// val and thresholds are 0-100
// ============================================================================

static void DrawInfoPanel(float px, float py,
    const char* title,
    int val, int warnT, int hotT,
    const char* suffix, bool ok)
{
    // Background + border
    FillRectGrad(px, py, px + ST_PANEL_W, py + ST_PANEL_H,
        D3DCOLOR_XRGB(16, 20, 50), D3DCOLOR_XRGB(10, 13, 34));
    HLine(py, px, px + ST_PANEL_W, COL_CYAN);
    HLine(py + ST_PANEL_H, px, px + ST_PANEL_W, COL_BORDER);
    VLine(px, py, py + ST_PANEL_H, COL_BORDER);
    VLine(px + ST_PANEL_W, py, py + ST_PANEL_H, COL_BORDER);

    DrawText(px + ST_P_PAD_X, py + ST_P_LBL_DY, title, 1.1f, COL_YELLOW);

    if (!ok)
    {
        DrawText(px + ST_P_PAD_X, py + ST_P_BIG_DY + 6.f, "ERR", 2.0f, COL_RED);
        return;
    }

    DWORD vc = ST_TempColor(val, warnT, hotT);

    char numBuf[12];  IntToStr(val, numBuf, sizeof(numBuf));
    char disp[16];    StrCat2(disp, sizeof(disp), numBuf, suffix);
    DrawText(px + ST_P_PAD_X, py + ST_P_BIG_DY, disp, ST_P_BIG_SC, vc);

    // Gauge bar
    float bx = px + ST_P_PAD_X;
    float by = py + ST_P_BAR_DY;
    float bw = ST_PANEL_W - ST_P_PAD_X * 2.f;
    FillRect(bx, by, bx + bw, by + ST_P_BAR_H, D3DCOLOR_XRGB(14, 17, 38));

    float fill = (float)val / 100.f; if (fill > 1.f) fill = 1.f;
    float warnF = (float)warnT / 100.f;
    float hotF = (float)hotT / 100.f;
    float fillX = bx + bw * fill;
    float warnX = bx + bw * warnF;
    float hotX = bx + bw * hotF;

    if (fill > 0.f)
    {
        if (fill <= warnF)
        {
            FillRectGrad(bx, by, fillX, by + ST_P_BAR_H,
                D3DCOLOR_XRGB(40, 200, 60), D3DCOLOR_XRGB(20, 120, 30));
        }
        else if (fill <= hotF)
        {
            FillRectGrad(bx, by, warnX, by + ST_P_BAR_H,
                D3DCOLOR_XRGB(40, 200, 60), D3DCOLOR_XRGB(20, 120, 30));
            FillRectGrad(warnX, by, fillX, by + ST_P_BAR_H,
                D3DCOLOR_XRGB(220, 160, 20), D3DCOLOR_XRGB(180, 100, 10));
        }
        else
        {
            FillRectGrad(bx, by, warnX, by + ST_P_BAR_H,
                D3DCOLOR_XRGB(40, 200, 60), D3DCOLOR_XRGB(20, 120, 30));
            FillRectGrad(warnX, by, hotX, by + ST_P_BAR_H,
                D3DCOLOR_XRGB(220, 160, 20), D3DCOLOR_XRGB(180, 100, 10));
            FillRectGrad(hotX, by, fillX, by + ST_P_BAR_H,
                D3DCOLOR_XRGB(255, 60, 30), D3DCOLOR_XRGB(180, 20, 10));
        }
    }

    // Threshold tick marks (only if warn < hot, i.e. normal orientation)
    if (warnT > 0 && warnT < hotT)
    {
        VLine(warnX, by - 2.f, by + ST_P_BAR_H + 2.f, COL_ORANGE);
        VLine(hotX, by - 2.f, by + ST_P_BAR_H + 2.f, COL_RED);
    }

    // Bar border
    HLine(by, bx, bx + bw, COL_BORDER);
    HLine(by + ST_P_BAR_H, bx, bx + bw, COL_BORDER);
    VLine(bx, by, by + ST_P_BAR_H, COL_BORDER);
    VLine(bx + bw, by, by + ST_P_BAR_H, COL_BORDER);
}

// ============================================================================
// DrawGraph — three-line scrolling history (CPU temp, load %, fan %)
// ============================================================================

static void DrawGraph()
{
    // Background
    FillRect(ST_GRAPH_X, ST_GRAPH_Y, ST_GRAPH_R, ST_GRAPH_B, D3DCOLOR_XRGB(10, 13, 30));

    // Danger / warm zone shading
    float dangerY = ST_GRAPH_Y + ST_GRAPH_H * (float)(GRAPH_MAX - CPU_HOT) / GRAPH_MAX;
    float warnY = ST_GRAPH_Y + ST_GRAPH_H * (float)(GRAPH_MAX - CPU_WARN) / GRAPH_MAX;
    FillRect(ST_GRAPH_X, ST_GRAPH_Y, ST_GRAPH_R, dangerY, D3DCOLOR_XRGB(40, 10, 10));
    FillRect(ST_GRAPH_X, dangerY, ST_GRAPH_R, warnY, D3DCOLOR_XRGB(28, 18, 8));

    // Grid lines every 10 units
    for (int t = 10; t < GRAPH_MAX; t += 10)
    {
        float gy = ST_GRAPH_B - ST_GRAPH_H * ((float)t / GRAPH_MAX);
        DWORD gc = (t == CPU_HOT) ? D3DCOLOR_XRGB(100, 30, 30)
            : (t == CPU_WARN) ? D3DCOLOR_XRGB(80, 60, 20)
            : D3DCOLOR_XRGB(25, 30, 55);
        HLine(gy, ST_GRAPH_X, ST_GRAPH_R, gc);

        char lbl[6]; IntToStr(t, lbl, sizeof(lbl));
        DrawText(ST_GRAPH_X - 24.f, gy - LINE_H * 0.5f, lbl, 0.95f,
            (t == CPU_HOT) ? COL_RED :
            (t == CPU_WARN) ? COL_ORANGE : COL_DIM);
    }

    // Border
    HLine(ST_GRAPH_Y, ST_GRAPH_X, ST_GRAPH_R, COL_BORDER);
    HLine(ST_GRAPH_B, ST_GRAPH_X, ST_GRAPH_R, COL_BORDER);
    VLine(ST_GRAPH_X, ST_GRAPH_Y, ST_GRAPH_B, COL_BORDER);
    VLine(ST_GRAPH_R, ST_GRAPH_Y, ST_GRAPH_B, COL_BORDER);

    if (s_histCount < 2) return;

    float xStep = ST_GRAPH_W / (float)(HISTORY_LEN - 1);

    for (int i = 0; i < s_histCount - 1; ++i)
    {
        int idxA = ST_HistIdx(i);
        int idxB = ST_HistIdx(i + 1);

        float xA = ST_GRAPH_R - (float)(s_histCount - 1 - i) * xStep;
        float xB = ST_GRAPH_R - (float)(s_histCount - 2 - i) * xStep;
        if (xA < ST_GRAPH_X) xA = ST_GRAPH_X;
        if (xB < ST_GRAPH_X) xB = ST_GRAPH_X;

        int steps = Ftoi(xB - xA);
        if (steps < 1) steps = 1;

        // CPU Temp — ST_TempColor matches panel value exactly
        {
            float yA = ST_GRAPH_B - ST_GRAPH_H * ((float)s_histCPU[idxA] / GRAPH_MAX);
            float yB = ST_GRAPH_B - ST_GRAPH_H * ((float)s_histCPU[idxB] / GRAPH_MAX);
            DWORD cc = ST_TempColor(
                (int)s_histCPU[idxB] > (int)s_histCPU[idxA]
                ? (int)s_histCPU[idxB] : (int)s_histCPU[idxA],
                CPU_WARN, CPU_HOT);
            for (int s = 0; s <= steps; ++s)
            {
                float t2 = (float)s / steps;
                float lx = xA + (xB - xA) * t2;
                float ly = yA + (yB - yA) * t2;
                if (lx >= ST_GRAPH_X && lx <= ST_GRAPH_R && ly >= ST_GRAPH_Y && ly <= ST_GRAPH_B)
                    FillRect(lx - 1.f, ly - 1.f, lx + 2.f, ly + 2.f, cc);
            }
        }

        // Load — green (matches CPU LOAD bar)
        {
            DWORD lc = D3DCOLOR_XRGB(220, 50, 220);
            float yA = ST_GRAPH_B - ST_GRAPH_H * ((float)s_histLoad[idxA] / GRAPH_MAX);
            float yB = ST_GRAPH_B - ST_GRAPH_H * ((float)s_histLoad[idxB] / GRAPH_MAX);
            for (int s = 0; s <= steps; ++s)
            {
                float t2 = (float)s / steps;
                float lx = xA + (xB - xA) * t2;
                float ly = yA + (yB - yA) * t2;
                if (lx >= ST_GRAPH_X && lx <= ST_GRAPH_R && ly >= ST_GRAPH_Y && ly <= ST_GRAPH_B)
                    FillRect(lx - 1.f, ly - 1.f, lx + 1.f, ly + 1.f, lc);
            }
        }

        // Fan — COL_ORANGE matches FAN SPEED panel value color
        {
            float yA = ST_GRAPH_B - ST_GRAPH_H * ((float)s_histFan[idxA] / GRAPH_MAX);
            float yB = ST_GRAPH_B - ST_GRAPH_H * ((float)s_histFan[idxB] / GRAPH_MAX);
            for (int s = 0; s <= steps; ++s)
            {
                float t2 = (float)s / steps;
                float lx = xA + (xB - xA) * t2;
                float ly = yA + (yB - yA) * t2;
                if (lx >= ST_GRAPH_X && lx <= ST_GRAPH_R && ly >= ST_GRAPH_Y && ly <= ST_GRAPH_B)
                    FillRect(lx, ly - 1.f, lx + 2.f, ly + 1.f, COL_ORANGE);
            }
        }
    }

    // Live dots at right edge
    if (s_histCount > 0 && s_sensorOK)
    {
        float dotX = ST_GRAPH_R - 3.f;
        float cpuY = ST_GRAPH_B - ST_GRAPH_H * ((float)s_curCPU / GRAPH_MAX);
        float loadV = (s_state == SSTATE_RUNNING) ? (float)s_measuredLoad : 0.f;
        float loadY = ST_GRAPH_B - ST_GRAPH_H * (loadV / GRAPH_MAX);
        FillRect(dotX - 3.f, cpuY - 3.f, dotX + 3.f, cpuY + 3.f,
            ST_TempColor((int)s_curCPU, CPU_WARN, CPU_HOT));
        FillRect(dotX - 3.f, loadY - 3.f, dotX + 3.f, loadY + 3.f,
            D3DCOLOR_XRGB(220, 50, 220));
        if (s_fanOK)
        {
            float fanY = ST_GRAPH_B - ST_GRAPH_H * ((float)((int)s_curFan * 2) / GRAPH_MAX);
            FillRect(dotX - 3.f, fanY - 3.f, dotX + 3.f, fanY + 3.f, COL_ORANGE);
        }
    }

    // ---- Legend (top-right inside graph) -----------------------------------
    float lx = ST_GRAPH_R - 130.f;
    float ly = ST_GRAPH_Y + 5.f;
    {
        DWORD cpuLegCol = (s_sensorOK && s_curCPU > 0)
            ? ST_TempColor((int)s_curCPU, CPU_WARN, CPU_HOT) : COL_GREEN;
        FillRect(lx, ly, lx + 10.f, ly + 5.f, cpuLegCol);
        DrawText(lx + 13.f, ly - 1.f, "CPU TEMP", 1.0f, cpuLegCol);
    }
    ly += 11.f;
    FillRect(lx, ly, lx + 10.f, ly + 5.f, D3DCOLOR_XRGB(220, 50, 220));
    DrawText(lx + 13.f, ly - 1.f, "LOAD %", 1.0f, D3DCOLOR_XRGB(220, 50, 220));
    ly += 11.f;
    FillRect(lx, ly, lx + 10.f, ly + 5.f, COL_ORANGE);
    DrawText(lx + 13.f, ly - 1.f, "FAN %", 1.0f, COL_ORANGE);

    // ---- Lower-left: runtime + min/max stats --------------------------------
    if (s_state == SSTATE_RUNNING && s_testStartMs > 0)
    {
        float bly = ST_GRAPH_B - 11.f;

        // Runtime mm:ss -- uses actual test start time, no sample-count cap
        DWORD secs = (GetTickCount() - s_testStartMs) / 1000;
        DWORD mm = secs / 60;
        DWORD ss = secs % 60;
        char mmBuf[8], ssBuf[8], timeBuf[24];
        IntToStr((int)mm, mmBuf, sizeof(mmBuf));
        IntToStr((int)ss, ssBuf, sizeof(ssBuf));
        StrCopy(timeBuf, sizeof(timeBuf), "RUN ");
        StrCat2(timeBuf, sizeof(timeBuf), timeBuf, mmBuf);
        StrCat2(timeBuf, sizeof(timeBuf), timeBuf, "m");
        if (ss < 10) StrCat2(timeBuf, sizeof(timeBuf), timeBuf, "0");
        StrCat2(timeBuf, sizeof(timeBuf), timeBuf, ssBuf);
        StrCat2(timeBuf, sizeof(timeBuf), timeBuf, "s");
        DrawText(ST_GRAPH_X + 4.f, bly, timeBuf, 1.0f, COL_DIM);

        // CPU min/max -- color matches panel (ST_TempColor of max)
        if (s_maxCPU > 0 && s_sensorOK)
        {
            char minBuf[8], maxBuf[8], cpuStat[24];
            IntToStr((int)s_minCPU, minBuf, sizeof(minBuf));
            IntToStr((int)s_maxCPU, maxBuf, sizeof(maxBuf));
            StrCopy(cpuStat, sizeof(cpuStat), "CPU ");
            StrCat2(cpuStat, sizeof(cpuStat), cpuStat, minBuf);
            StrCat2(cpuStat, sizeof(cpuStat), cpuStat, "-");
            StrCat2(cpuStat, sizeof(cpuStat), cpuStat, maxBuf);
            StrCat2(cpuStat, sizeof(cpuStat), cpuStat, "C");
            DrawText(ST_GRAPH_X + 80.f, bly, cpuStat, 1.0f,
                ST_TempColor((int)s_maxCPU, CPU_WARN, CPU_HOT));
        }

        // Fan min/max
        if (s_maxFan > 0 && s_fanOK)
        {
            char minBuf[8], maxBuf[8], fanStat[24];
            IntToStr((int)s_minFan, minBuf, sizeof(minBuf));
            IntToStr((int)s_maxFan, maxBuf, sizeof(maxBuf));
            StrCopy(fanStat, sizeof(fanStat), "FAN ");
            StrCat2(fanStat, sizeof(fanStat), fanStat, minBuf);
            StrCat2(fanStat, sizeof(fanStat), fanStat, "-");
            StrCat2(fanStat, sizeof(fanStat), fanStat, maxBuf);
            StrCat2(fanStat, sizeof(fanStat), fanStat, "%");
            DrawText(ST_GRAPH_X + 180.f, bly, fanStat, 1.0f, COL_ORANGE);
        }

        // SSE1 status — bottom right of graph
        if (StressMath_SSEEnabled())
            DrawText(ST_GRAPH_R - 84.f, bly, "SSE1 ON", 1.0f, D3DCOLOR_XRGB(80, 200, 255));
        else
            DrawText(ST_GRAPH_R - 84.f, bly, "SSE1 OFF", 1.0f, COL_DIM);
    }
}

// ============================================================================
// DrawThresholdOverlay — temp limit picker, shown before confirm
// ============================================================================

static void DrawThresholdOverlay()
{
    float ow = 380.f;
    float oh = 140.f;
    float ox = SW * 0.5f - ow * 0.5f;
    float oy = SH * 0.5f - oh * 0.5f;

    FillRectGrad(ox, oy, ox + ow, oy + oh,
        D3DCOLOR_XRGB(20, 28, 70), D3DCOLOR_XRGB(12, 16, 46));
    HLine(oy, ox, ox + ow, COL_CYAN);
    HLine(oy + oh, ox, ox + ow, COL_CYAN);
    VLine(ox, oy, oy + oh, COL_BORDER);
    VLine(ox + ow, oy, oy + oh, COL_BORDER);

    const char* t1 = "SET THERMAL ABORT LIMIT";
    DrawText(ox + (ow - TW(t1, 1.25f)) * 0.5f, oy + 8.f, t1, 1.25f, COL_YELLOW);

    // Big temperature value
    char tStr[12];  IntToStr(s_tempThreshold, tStr, sizeof(tStr));
    char tDisp[16]; StrCopy(tDisp, sizeof(tDisp), tStr);
    StrCat2(tDisp, sizeof(tDisp), tDisp, "C");

    DWORD tCol = (s_tempThreshold >= 85) ? COL_RED
        : (s_tempThreshold >= 75) ? COL_ORANGE
        : COL_GREEN;

    float valW = TW(tDisp, 2.6f);
    DrawText(ox + (ow - valW) * 0.5f, oy + 34.f, tDisp, 2.6f, tCol);

    // Arrow hints flanking the value
    DrawText(ox + (ow - valW) * 0.5f - 22.f, oy + 42.f, "<", 2.0f, COL_GRAY);
    DrawText(ox + (ow + valW) * 0.5f + 6.f, oy + 42.f, ">", 2.0f, COL_GRAY);

    // Split hint across two lines so it fits the box
    const char* h1 = "[LT] Decrease    [RT] Increase";
    const char* h2 = "[A] Continue    [B] Cancel";
    DrawText(ox + (ow - TW(h1, 1.0f)) * 0.5f, oy + oh - 30.f, h1, 1.0f, COL_GRAY);
    DrawText(ox + (ow - TW(h2, 1.0f)) * 0.5f, oy + oh - 14.f, h2, 1.0f, COL_GRAY);
}

// ============================================================================
// DrawFanOverlay — fan speed picker, same modal pattern as threshold overlay
// ============================================================================

static void DrawFanOverlay()
{
    float ow = 400.f;
    float oh = 160.f;
    float ox = SW * 0.5f - ow * 0.5f;
    float oy = SH * 0.5f - oh * 0.5f;

    FillRectGrad(ox, oy, ox + ow, oy + oh,
        D3DCOLOR_XRGB(20, 28, 70), D3DCOLOR_XRGB(12, 16, 46));
    HLine(oy, ox, ox + ow, COL_CYAN);
    HLine(oy + oh, ox, ox + ow, COL_CYAN);
    VLine(ox, oy, oy + oh, COL_BORDER);
    VLine(ox + ow, oy, oy + oh, COL_BORDER);

    const char* title = "SET FAN SPEED";
    DrawText(ox + (ow - TW(title, 1.25f)) * 0.5f, oy + 8.f, title, 1.25f, COL_YELLOW);

    // ---- Mode row: AUTO / MANUAL pill ----------------------------------------
    float modeY = oy + 32.f;
    float pillW = 70.f;
    float pillH = 20.f;
    float autoX = ox + ow * 0.5f - pillW - 8.f;
    float manX = ox + ow * 0.5f + 8.f;

    // AUTO pill
    FillRectGrad(autoX, modeY, autoX + pillW, modeY + pillH,
        s_fanAuto ? D3DCOLOR_XRGB(30, 100, 60) : D3DCOLOR_XRGB(18, 25, 55),
        s_fanAuto ? D3DCOLOR_XRGB(16, 60, 30) : D3DCOLOR_XRGB(12, 16, 38));
    HLine(modeY, autoX, autoX + pillW, s_fanAuto ? COL_GREEN : COL_BORDER);
    HLine(modeY + pillH, autoX, autoX + pillW, s_fanAuto ? COL_GREEN : COL_BORDER);
    VLine(autoX, modeY, modeY + pillH, s_fanAuto ? COL_GREEN : COL_BORDER);
    VLine(autoX + pillW, modeY, modeY + pillH, s_fanAuto ? COL_GREEN : COL_BORDER);
    DrawText(autoX + (pillW - TW("AUTO", 1.1f)) * 0.5f, modeY + 4.f,
        "AUTO", 1.1f, s_fanAuto ? COL_GREEN : COL_DIM);

    // MANUAL pill
    FillRectGrad(manX, modeY, manX + pillW, modeY + pillH,
        !s_fanAuto ? D3DCOLOR_XRGB(60, 40, 10) : D3DCOLOR_XRGB(18, 25, 55),
        !s_fanAuto ? D3DCOLOR_XRGB(35, 22, 5) : D3DCOLOR_XRGB(12, 16, 38));
    HLine(modeY, manX, manX + pillW, !s_fanAuto ? COL_ORANGE : COL_BORDER);
    HLine(modeY + pillH, manX, manX + pillW, !s_fanAuto ? COL_ORANGE : COL_BORDER);
    VLine(manX, modeY, modeY + pillH, !s_fanAuto ? COL_ORANGE : COL_BORDER);
    VLine(manX + pillW, modeY, modeY + pillH, !s_fanAuto ? COL_ORANGE : COL_BORDER);
    DrawText(manX + (pillW - TW("MANUAL", 1.1f)) * 0.5f, modeY + 4.f,
        "MANUAL", 1.1f, !s_fanAuto ? COL_ORANGE : COL_DIM);

    // ---- Value row -----------------------------------------------------------
    float valY = oy + 64.f;

    if (s_fanAuto)
    {
        const char* autoMsg = "SMC controls fan speed";
        DrawText(ox + (ow - TW(autoMsg, 1.1f)) * 0.5f, valY + 10.f, autoMsg, 1.1f, COL_GRAY);
    }
    else
    {
        // Big percentage value
        char pBuf[8];  IntToStr(s_fanTargetPct, pBuf, sizeof(pBuf));
        char pDisp[12]; StrCopy(pDisp, sizeof(pDisp), pBuf);
        StrCat2(pDisp, sizeof(pDisp), pDisp, "%");

        DWORD pCol = (s_fanTargetPct >= 80) ? COL_RED
            : (s_fanTargetPct >= 50) ? COL_ORANGE
            : COL_GREEN;

        float valW = TW(pDisp, 2.6f);
        DrawText(ox + (ow - valW) * 0.5f, valY, pDisp, 2.6f, pCol);

        // Arrow hints flanking value
        DrawText(ox + (ow - valW) * 0.5f - 22.f, valY + 8.f, "<", 2.0f, COL_GRAY);
        DrawText(ox + (ow + valW) * 0.5f + 6.f, valY + 8.f, ">", 2.0f, COL_GRAY);
    }

    // ---- Hint rows ----------------------------------------------------------
    const char* h1 = s_fanAuto
        ? "[X] Switch to Manual"
        : "[LT] -5%    [RT] +5%    [X] Switch to Auto";
    const char* h2 = "[A] Next    [B] Back";
    DrawText(ox + (ow - TW(h1, 1.0f)) * 0.5f, oy + oh - 30.f, h1, 1.0f, COL_GRAY);
    DrawText(ox + (ow - TW(h2, 1.0f)) * 0.5f, oy + oh - 14.f, h2, 1.0f, COL_GRAY);
}
// ============================================================================

static void DrawConfirmOverlay()
{
    float ox = SW * 0.5f - 180.f;
    float oy = SH * 0.5f - 62.f;
    float ow = 360.f;
    float oh = 124.f;

    FillRectGrad(ox, oy, ox + ow, oy + oh,
        D3DCOLOR_XRGB(20, 28, 70), D3DCOLOR_XRGB(12, 16, 46));
    HLine(oy, ox, ox + ow, COL_CYAN);
    HLine(oy + oh, ox, ox + ow, COL_CYAN);
    VLine(ox, oy, oy + oh, COL_CYAN);
    VLine(ox + ow, oy, oy + oh, COL_CYAN);

    const char* t1 = "START CPU STRESS TEST?";
    DrawText(ox + (ow - TW(t1, 1.3f)) * 0.5f, oy + 8.f, t1, 1.3f, COL_WHITE);

    // Settings recap — show what the wizard configured
    {
        // Temp threshold
        char thrBuf[16]; char thrNum[8];
        IntToStr(s_tempThreshold, thrNum, sizeof(thrNum));
        StrCopy(thrBuf, sizeof(thrBuf), "ABORT TEMP: ");
        StrCat2(thrBuf, sizeof(thrBuf), thrBuf, thrNum);
        StrCat2(thrBuf, sizeof(thrBuf), thrBuf, "C");
        DWORD thrCol = (s_tempThreshold >= 85) ? COL_RED
            : (s_tempThreshold >= 75) ? COL_ORANGE : COL_GREEN;
        DrawText(ox + (ow - TW(thrBuf, 1.1f)) * 0.5f, oy + 32.f, thrBuf, 1.1f, thrCol);

        // Fan setting
        char fanBuf[24];
        if (s_fanAuto)
        {
            StrCopy(fanBuf, sizeof(fanBuf), "FAN: AUTO (SMC)");
        }
        else
        {
            char pctNum[8];
            IntToStr(s_fanTargetPct, pctNum, sizeof(pctNum));
            StrCopy(fanBuf, sizeof(fanBuf), "FAN: MANUAL ");
            StrCat2(fanBuf, sizeof(fanBuf), fanBuf, pctNum);
            StrCat2(fanBuf, sizeof(fanBuf), fanBuf, "%");
        }
        DWORD fanCol = s_fanAuto ? COL_GREEN : COL_ORANGE;
        DrawText(ox + (ow - TW(fanBuf, 1.1f)) * 0.5f, oy + 50.f, fanBuf, 1.1f, fanCol);
    }

    const char* t2 = "Hold  LT + RT  simultaneously";
    DrawText(ox + (ow - TW(t2, 1.15f)) * 0.5f, oy + 74.f, t2, 1.15f, COL_YELLOW);

    const char* t3 = "to begin the test";
    DrawText(ox + (ow - TW(t3, 1.15f)) * 0.5f, oy + 90.f, t3, 1.15f, COL_YELLOW);

    const char* t4 = "[B]  Cancel";
    DrawText(ox + (ow - TW(t4, 1.05f)) * 0.5f, oy + oh - 16.f, t4, 1.05f, COL_GRAY);
}

// ============================================================================
// DrawAbortBar — hold-progress shown when Back+A held during RUNNING
// ============================================================================

static void DrawAbortBar(DWORD holdMs)
{
    float frac = (float)holdMs / (float)ABORT_HOLD_MS;
    if (frac > 1.f) frac = 1.f;

    float bx = LM;
    float bw = SW - LM * 2.f;
    float by = ST_ABORT_BAR_Y;

    FillRect(bx, by, bx + bw, by + ST_ABORT_BAR_H, D3DCOLOR_XRGB(30, 10, 10));

    if (frac > 0.f)
    {
        FillRectGrad(bx, by, bx + bw * frac, by + ST_ABORT_BAR_H,
            D3DCOLOR_XRGB(255, 60, 30), D3DCOLOR_XRGB(200, 20, 10));
    }

    HLine(by, bx, bx + bw, COL_BORDER);
    HLine(by + ST_ABORT_BAR_H, bx, bx + bw, COL_BORDER);

    DWORD remain = (holdMs < ABORT_HOLD_MS)
        ? (ABORT_HOLD_MS - holdMs) / 1000 + 1
        : 0;
    char remBuf[40];
    StrCopy(remBuf, sizeof(remBuf), "ABORTING IN ");
    char remSec[8]; IntToStr((int)remain, remSec, sizeof(remSec));
    StrCat2(remBuf, sizeof(remBuf), remBuf, remSec);
    StrCat2(remBuf, sizeof(remBuf), remBuf, "s  --  RELEASE TO CANCEL");
    // Centre the label above the bar
    float lblX = LM + (ST_LOAD_BAR_W - TW(remBuf, 1.05f)) * 0.5f;
    DrawText(lblX, by - 13.f, remBuf, 1.05f, COL_RED);
}

// ============================================================================
// ST_DrawTabStrip
// ============================================================================

void ST_DrawTabStrip(StressCard active)
{
    bool cpuSel = (active == CARD_CPU);
    bool ramSel = (active == CARD_RAM);
    bool gpuSel = (active == CARD_GPU);

    // CPU tab
    FillRectGrad(ST_TAB_CPU_X, ST_TAB_Y, ST_TAB_CPU_X + ST_TAB_W, ST_TAB_Y + ST_TAB_H,
        cpuSel ? D3DCOLOR_XRGB(40, 80, 160) : D3DCOLOR_XRGB(18, 25, 55),
        cpuSel ? D3DCOLOR_XRGB(20, 50, 110) : D3DCOLOR_XRGB(12, 16, 38));
    HLine(ST_TAB_Y, ST_TAB_CPU_X, ST_TAB_CPU_X + ST_TAB_W,
        cpuSel ? COL_CYAN : COL_BORDER);
    DrawText(ST_TAB_CPU_X + 8.f, ST_TAB_Y + 3.f, "CPU", 1.15f,
        cpuSel ? COL_WHITE : COL_DIM);

    // RAM tab
    FillRectGrad(ST_TAB_RAM_X, ST_TAB_Y, ST_TAB_RAM_X + ST_TAB_W, ST_TAB_Y + ST_TAB_H,
        ramSel ? D3DCOLOR_XRGB(40, 80, 160) : D3DCOLOR_XRGB(18, 25, 55),
        ramSel ? D3DCOLOR_XRGB(20, 50, 110) : D3DCOLOR_XRGB(12, 16, 38));
    HLine(ST_TAB_Y, ST_TAB_RAM_X, ST_TAB_RAM_X + ST_TAB_W,
        ramSel ? COL_CYAN : COL_BORDER);
    DrawText(ST_TAB_RAM_X + 8.f, ST_TAB_Y + 3.f, "RAM", 1.15f,
        ramSel ? COL_WHITE : COL_DIM);

    // GPU tab
    FillRectGrad(ST_TAB_GPU_X, ST_TAB_Y, ST_TAB_GPU_X + ST_TAB_W, ST_TAB_Y + ST_TAB_H,
        gpuSel ? D3DCOLOR_XRGB(40, 80, 160) : D3DCOLOR_XRGB(18, 25, 55),
        gpuSel ? D3DCOLOR_XRGB(20, 50, 110) : D3DCOLOR_XRGB(12, 16, 38));
    HLine(ST_TAB_Y, ST_TAB_GPU_X, ST_TAB_GPU_X + ST_TAB_W,
        gpuSel ? COL_CYAN : COL_BORDER);
    DrawText(ST_TAB_GPU_X + 8.f, ST_TAB_Y + 3.f, "GPU", 1.15f,
        gpuSel ? COL_WHITE : COL_DIM);

    // Bottom rule under all tabs
    HLine(ST_TAB_Y + ST_TAB_H, LM, SW - LM, COL_BORDER);
}

// ============================================================================
// RenderCPUCard
// ============================================================================

static void RenderCPUCard(const DiagLogo& logo)
{
    const char* hint =
        (s_state == SSTATE_IDLE) ? "[A] Start Test    [B] Back    [Right] RAM / GPU" :
        (s_state == SSTATE_THRESHOLD) ? "[LT] Lower    [RT] Raise    [A] Next    [B] Cancel" :
        (s_state == SSTATE_FAN) ? "[X] Auto/Manual    [LT/RT] Adjust    [A] Next    [B] Back" :
        (s_state == SSTATE_CONFIRM) ? "[LT+RT] Confirm    [B] Cancel" :
        "[Right] RAM / GPU    Hold [Back+A] 5s to Abort";

    DrawPageChrome(logo, "STRESS TEST  -  CPU", hint);
    ST_DrawTabStrip(CARD_CPU);

    // ---- Info panels ----
    DrawInfoPanel(ST_PANEL_T_X, ST_PANEL_TOP, "CPU TEMP",
        (int)s_curCPU, CPU_WARN, CPU_HOT, "C", s_sensorOK);

    DrawInfoPanel(ST_PANEL_F_X, ST_PANEL_TOP, "FAN SPEED",
        s_fanOK ? (int)s_curFan * 2 : 0,
        0, 100, "%", s_fanOK);

    // MHz stat box — no gauge, just a large readout
    {
        float px = ST_PANEL_M_X;
        float py = ST_PANEL_TOP;
        float pw = ST_PANEL_W;
        FillRectGrad(px, py, px + pw, py + ST_PANEL_H,
            D3DCOLOR_XRGB(16, 20, 50), D3DCOLOR_XRGB(10, 13, 34));
        HLine(py, px, px + pw, COL_CYAN);
        HLine(py + ST_PANEL_H, px, px + pw, COL_BORDER);
        VLine(px, py, py + ST_PANEL_H, COL_BORDER);
        VLine(px + pw, py, py + ST_PANEL_H, COL_BORDER);

        DrawText(px + ST_P_PAD_X, py + ST_P_LBL_DY, "CPU SPEED", 1.1f, COL_YELLOW);

        char mhzBuf[12]; IntToStr(s_curMHz, mhzBuf, sizeof(mhzBuf));
        char mhzDisp[20]; StrCat2(mhzDisp, sizeof(mhzDisp), mhzBuf, " MHz");
        DrawText(px + ST_P_PAD_X, py + ST_P_BIG_DY, mhzDisp, ST_P_BIG_SC, COL_WHITE);

        const char* sLabel = "MCPX PLL + MSR";
        DrawText(px + ST_P_PAD_X, py + ST_PANEL_H - 14.f, sLabel, 1.0f, COL_DIM);
    }

    // Thermal abort notice and running badge — drawn AFTER the MHz panel so
    // the panel FillRectGrad does not paint over them.
    // DrawTextR anchor stays at SW-LM exactly as original.
    if (s_state == SSTATE_IDLE && s_thermalAbort)
    {
        DrawTextR(SW - LM, ST_PANEL_TOP + 2.f, "! THERMAL ABORT !", 1.25f, COL_RED);
        char thrBuf[24];
        StrCopy(thrBuf, sizeof(thrBuf), "LIMIT: ");
        char thrVal[8]; IntToStr(s_tempThreshold, thrVal, sizeof(thrVal));
        StrCat2(thrBuf, sizeof(thrBuf), thrBuf, thrVal);
        StrCat2(thrBuf, sizeof(thrBuf), thrBuf, "C reached");
        DrawTextR(SW - LM, ST_PANEL_TOP + 16.f, thrBuf, 1.1f, COL_ORANGE);
    }

    if (s_state == SSTATE_RUNNING)
    {
        DWORD secs = (GetTickCount() - s_testStartMs) / 1000;
        char elBuf[32];
        StrCopy(elBuf, sizeof(elBuf), "ELAPSED ");
        char secStr[12]; IntToStr((int)secs, secStr, sizeof(secStr));
        StrCat2(elBuf, sizeof(elBuf), elBuf, secStr);
        StrCat2(elBuf, sizeof(elBuf), elBuf, "s");

        DrawTextR(SW - LM, ST_PANEL_TOP + 2.f, "* RUNNING *", 1.25f, COL_RED);
        DrawTextR(SW - LM, ST_PANEL_TOP + 16.f, elBuf, 1.1f, COL_GRAY);
        char thrLine[20];
        StrCopy(thrLine, sizeof(thrLine), "ABORT @ ");
        char thrV[8]; IntToStr(s_tempThreshold, thrV, sizeof(thrV));
        StrCat2(thrLine, sizeof(thrLine), thrLine, thrV);
        StrCat2(thrLine, sizeof(thrLine), thrLine, "C");
        DrawTextR(SW - LM, ST_PANEL_TOP + 30.f, thrLine, 1.0f, COL_ORANGE);
    }

    // ---- CPU Load bar ----
    {
        int loadPct = (s_state == SSTATE_RUNNING) ? (int)s_measuredLoad : 0;

        DrawText(ST_LOAD_BAR_X, ST_LOAD_LBL_Y, "CPU LOAD", 1.1f, COL_YELLOW);

        float bx = ST_LOAD_BAR_X;
        float by = ST_LOAD_BAR_Y;
        float bw = ST_LOAD_BAR_W;

        FillRect(bx, by, bx + bw, by + ST_LOAD_BAR_H, D3DCOLOR_XRGB(14, 17, 38));

        if (loadPct > 0)
        {
            float fillW = bw * (loadPct / 100.f);
            if (fillW > bw) fillW = bw;
            FillRectGrad(bx, by, bx + fillW, by + ST_LOAD_BAR_H,
                D3DCOLOR_XRGB(220, 50, 220), D3DCOLOR_XRGB(140, 20, 140));
        }

        char pctBuf[8]; IntToStr(loadPct, pctBuf, sizeof(pctBuf));
        char pctDisp[12]; StrCat2(pctDisp, sizeof(pctDisp), pctBuf, "%");
        float lblX = bx + (bw - TW(pctDisp, 1.15f)) * 0.5f;
        DrawText(lblX, by + 1.f, pctDisp, 1.15f, COL_WHITE);

        HLine(by, bx, bx + bw, COL_BORDER);
        HLine(by + ST_LOAD_BAR_H, bx, bx + bw, COL_BORDER);
        VLine(bx, by, by + ST_LOAD_BAR_H, COL_BORDER);
        VLine(bx + bw, by, by + ST_LOAD_BAR_H, COL_BORDER);
    }

    // ---- Graph ----
    HLine(ST_GRAPH_HDR_Y, LM, SW - LM, COL_BORDER);
    DrawText(LM, ST_GRAPH_HDR_Y + 2.f, "LIVE TELEMETRY", 1.1f, COL_YELLOW);
    DrawGraph();

    // ---- Abort UI (only when running) ----
    if (s_state == SSTATE_RUNNING)
    {
        if (s_abortHolding)
            DrawAbortBar(GetTickCount() - s_abortHoldStart);
        else
            DrawText(LM, ST_ABORT_BAR_Y - 2.f,
                "Hold  [Back + A]  for 5s to abort",
                1.0f, COL_DIM);
    }

    // ---- Threshold picker, confirm overlay, fan overlay (drawn last, on top) ----
    if (s_state == SSTATE_THRESHOLD)
        DrawThresholdOverlay();
    else if (s_state == SSTATE_CONFIRM)
        DrawConfirmOverlay();
    else if (s_state == SSTATE_FAN)
        DrawFanOverlay();
}

// ============================================================================
// Public API wrappers
// ============================================================================

void ST_CPU_TakeSample() { TakeSample(); }
void ST_CPU_ReadMHz() { s_curMHz = ReadCPUMHz(); }
void ST_CPU_Render(const DiagLogo& logo) { RenderCPUCard(logo); }

// Fan overlay input helpers — called from StressTest.cpp tick
void ST_CPU_FanToggleAuto()
{
    s_fanAuto = !s_fanAuto;
}

void ST_CPU_FanStep(int delta)
{
    // delta is +1 or -1 representing one 5% step
    if (s_fanAuto) return;
    s_fanTargetPct += delta * 5;
    if (s_fanTargetPct < 0)   s_fanTargetPct = 0;
    if (s_fanTargetPct > 100) s_fanTargetPct = 100;
}

void ST_CPU_FanApply()
{
    if (s_fanAuto)
    {
        // Release back to SMC automatic control:
        // Write 0 to fan mode reg 0x05 first, then 0 to speed reg 0x06.
        SMBusWrite(SMBADDR_PIC, 0x05, 0);
        SMBusWrite(SMBADDR_PIC, 0x06, 0);
    }
    else
    {
        // Manual override — per xboxdevwiki PIC register table:
        //   0x05 = fan mode: 0=automatic, 1=custom speed from reg 0x06
        //   0x06 = fan speed: 0..50 (multiply by 2 for 0-100%)
        // Mode MUST be set to 1 before the speed write or the SMC ignores it.
        BYTE raw = (BYTE)(s_fanTargetPct / 2);
        SMBusWrite(SMBADDR_PIC, 0x05, 1);
        SMBusWrite(SMBADDR_PIC, 0x06, raw);
    }
}

void ST_CPU_FanCancel()
{
    // Cancel without applying — mode reverts to auto, SMC resumes control.
    s_fanAuto = true;
}

void ST_CPU_FanRelease()
{
    // Called on module exit — restore SMC automatic fan control.
    // Must write 0 to mode reg 0x05 to hand control back to the SMC.
    s_fanAuto = true;
    SMBusWrite(SMBADDR_PIC, 0x05, 0);
    SMBusWrite(SMBADDR_PIC, 0x06, 0);
}

bool ST_CPU_FanIsAuto() { return s_fanAuto; }
int  ST_CPU_FanGetPct() { return s_fanTargetPct; }