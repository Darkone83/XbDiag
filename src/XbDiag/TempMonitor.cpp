// TempMonitor.cpp
// XbDiag - Temperature Monitor
//
// Layout (640x480 design space):
//
//  [TOP BAR]
//  ----------------------------------------------------------------
//  LEFT COLUMN (x=LM..308)          RIGHT COLUMN (x=322..SW-LM)
//
//  AMBIENT (board/GPU area)          CPU DIE
//  +--------------------------+      +--------------------------+
//  |  Large temp readout      |      |  Large temp readout      |
//  |  Horizontal gauge bar    |      |  Horizontal gauge bar    |
//  |  Status (OK/WARM/HOT)    |      |  Status (OK/WARM/HOT)    |
//  +--------------------------+      +--------------------------+
//
//  HISTORY GRAPH (full width, scrolling line graph)
//  128 samples, ambient=cyan line, CPU=yellow line
//  Y axis: 0-100C  grid lines at 10C intervals
//  X axis: scrolls left as new samples arrive
//  Danger zones shaded red (>65C ambient, >80C CPU)
//
//  [BOT BAR]  [B] Back    refresh rate indicator
//
// Thermal read strategy (cross-revision safe):
//
//   1.0 - 1.5:  ADM1032 at SMBus 0x4C
//                 reg 0x00 = local die (board/MCPX area)
//                 reg 0x01 = remote diode (CPU die)
//               Detected by: SMBusRead(SMBADDR_ADM1032, 0x00) succeeds
//
//   1.6:        ADM1032 removed; CPU temp inside Xcalibur.
//               PIC mirrors both temps via:
//                 reg 0x09 = CPU temperature  (°C)
//                 reg 0x0A = board temperature (°C)
//               Xcalibur readings are noisy — average 10 samples.
//
//   Fallback:   If neither path responds, show friendly wait message.
//
// Thresholds:
//   Ambient:  <50=OK  50-65=WARM  >65=HOT
//   CPU:      <65=OK  65-80=WARM  >80=HOT
//
// Sample rate: ~500ms  |  Graph: 128 samples (~64 seconds)
// NOTE: Legacy code used 0x58 (software-shifted ADM1032 addr). Correct
//       hardware address is 0x4C (HalReadSMBusValue uses hardware addr).

#include "TempMonitor.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

// ============================================================================
// Config
// ============================================================================

static const int   HISTORY_LEN = 128;
static const DWORD SAMPLE_INTERVAL_MS = 500;

// Thresholds (Celsius)
static const int AMB_WARN = 50;
static const int AMB_HOT = 65;
static const int CPU_WARN = 65;
static const int CPU_HOT = 80;
static const int GRAPH_MAX = 100;   // top of Y axis

// ============================================================================
// Layout constants (design space 640x480)
// ============================================================================

// Two gauge panels side by side
static const float COL_W = 290.f;
static const float COL_L_X = LM;               // 32
static const float COL_R_X = SW - LM - COL_W;  // 318
static const float GAUGE_Y = CONTENT_Y + 6.f;  // 64
static const float GAUGE_H = 88.f;

// Big temperature number
static const float BIG_TEMP_SCALE = 3.5f;
static const float BIG_X_OFF = 10.f;
static const float BIG_Y_OFF = 8.f;

// Gauge bar
static const float BAR_X_OFF = 10.f;
static const float BAR_Y_OFF = 52.f;
static const float BAR_H = 14.f;
static const float BAR_W = COL_W - 20.f;

// Status label
static const float STATUS_Y_OFF = 70.f;

// Graph area
static const float GRAPH_X = LM + 28.f;       // left of plot area (room for Y labels)
static const float GRAPH_Y = GAUGE_Y + GAUGE_H + 18.f;  // ~170
static const float GRAPH_W = SW - GRAPH_X - LM - 2.f;   // ~578
static const float GRAPH_H = BOT_BAR_Y - GRAPH_Y - 28.f; // ~224
static const float GRAPH_R = GRAPH_X + GRAPH_W;
static const float GRAPH_B = GRAPH_Y + GRAPH_H;

// ============================================================================
// State
// ============================================================================

static BYTE  s_histAmb[HISTORY_LEN];   // ring buffer
static BYTE  s_histCPU[HISTORY_LEN];
static int   s_histHead;               // index of oldest sample
static int   s_histCount;              // how many valid samples
static DWORD s_lastSample;
static WORD  s_prevBtns;

static BYTE  s_curAmb;
static BYTE  s_curCPU;
static BYTE  s_curFan;       // 0-50 raw from PIC reg 0x10; display as (val*2)%
static bool  s_fanDetected;  // true if PIC fan readback responded
static bool  s_sensorOK;

// Sensor path detected on first successful read
enum SensorPath { PATH_UNKNOWN = 0, PATH_ADM1032, PATH_PIC_16 };
static SensorPath s_path = PATH_UNKNOWN;

// Set true when PATH_PIC_16 is active but 0x09/0x0A NAK —
// Xyclops (1.6 SMC) does not expose temp registers at these offsets.
// Shows a targeted informational message rather than a generic error.
static bool s_xyclopsNoTemp = false;

// 10-sample accumulator for 1.6 PIC averaging
static int  s_avg_amb_acc = 0;
static int  s_avg_cpu_acc = 0;
static int  s_avg_count = 0;
static const int AVG_SAMPLES = 10;

// ============================================================================
// Helpers
// ============================================================================

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

static DWORD TempColor(int temp, int warn, int hot)
{
    if (temp >= hot)  return COL_RED;
    if (temp >= warn) return COL_ORANGE;
    return COL_GREEN;
}

static const char* TempStatus(int temp, int warn, int hot)
{
    if (temp >= hot)  return "HOT";
    if (temp >= warn) return "WARM";
    return "OK";
}

static int HistSample(int indexFromOldest)
{
    // Returns ring buffer index for sample [indexFromOldest] (0=oldest)
    return (s_histHead + indexFromOldest) % HISTORY_LEN;
}

// ============================================================================
// Sample
// ============================================================================

static void TakeSample()
{
    BYTE amb = 0, cpu = 0;
    bool ok = false;

    // --- Auto-detect sensor path on first call ---
    if (s_path == PATH_UNKNOWN)
    {
        BYTE probe = 0;
        if (SMBusRead(SMBADDR_ADM1032, 0x00, probe))
            s_path = PATH_ADM1032;
        else
            s_path = PATH_PIC_16;  // ADM1032 absent (1.6)
    }

    if (s_path == PATH_ADM1032)
    {
        // 1.0-1.5: ADM1032 at 0x4C, regs 0x00 (local) + 0x01 (remote/CPU)
        BYTE admFrac = 0;
        ok = SMBusRead(SMBADDR_ADM1032, 0x00, amb) &&
            SMBusRead(SMBADDR_ADM1032, 0x01, cpu);
        // Read fractional byte for CPU temp (reg 0x10), adds sub-degree precision
        // cpu_precise = cpu + admFrac/256  (ref: PrometheOS temperatureManager)
        if (ok) SMBusRead(SMBADDR_ADM1032, 0x10, admFrac);
    }
    else
    {
        // 1.6: Attempt PIC/Xyclops regs 0x09 (CPU) + 0x0A (board).
        // On 1.0-1.5 the PIC16LC proxies ADM1032 temps at these offsets.
        // On 1.6 the SMC is Xyclops (8051 core) which does NOT implement
        // these registers — they NAK. Detect this and flag gracefully
        // rather than showing a generic sensor error.
        BYTE picCPU = 0, picAmb = 0;
        ok = SMBusRead(SMBADDR_PIC, 0x09, picCPU) &&
            SMBusRead(SMBADDR_PIC, 0x0A, picAmb);

        if (!ok)
        {
            // Xyclops temp registers not available — suppress generic error,
            // show informational N/A message instead.
            s_xyclopsNoTemp = true;
            s_sensorOK = false;
            return;
        }

        s_xyclopsNoTemp = false;

        // Apply 0.8x scaling to ambient (ref: PrometheOS xboxConfig)
        picAmb = (BYTE)((int)picAmb * 4 / 5);

        s_avg_cpu_acc += (int)picCPU;
        s_avg_amb_acc += (int)picAmb;
        ++s_avg_count;
        if (s_avg_count < AVG_SAMPLES)
            return;   // accumulate more samples before committing
        cpu = (BYTE)(s_avg_cpu_acc / AVG_SAMPLES);
        amb = (BYTE)(s_avg_amb_acc / AVG_SAMPLES);
        s_avg_cpu_acc = 0;
        s_avg_amb_acc = 0;
        s_avg_count = 0;
    }

    s_sensorOK = ok;
    if (!s_sensorOK) return;

    s_curAmb = amb;
    s_curCPU = cpu;

    // Fan speed readback: PIC reg 0x10, range 0-50 (maps to 0-100%)
    // Present on all revisions that have a PIC (all retail Xbox)
    {
        BYTE fanRaw = 0;
        if (SMBusRead(SMBADDR_PIC, 0x10, fanRaw))
        {
            s_curFan = fanRaw;
            s_fanDetected = true;
        }
        // else: leave previous value; s_fanDetected stays false until first success
    }

    // Write into ring buffer
    int writeIdx;
    if (s_histCount < HISTORY_LEN)
    {
        writeIdx = (s_histHead + s_histCount) % HISTORY_LEN;
        ++s_histCount;
    }
    else
    {
        writeIdx = s_histHead;
        s_histHead = (s_histHead + 1) % HISTORY_LEN;
    }
    s_histAmb[writeIdx] = amb;
    s_histCPU[writeIdx] = cpu;
}

// ============================================================================
// OnEnter
// ============================================================================

void TempMonitor_OnEnter()
{
    s_prevBtns = 0;
    s_histHead = 0;
    s_histCount = 0;
    s_curAmb = 0;
    s_curCPU = 0;
    s_curFan = 0;
    s_fanDetected = false;
    s_sensorOK = false;
    s_lastSample = GetTickCount() - SAMPLE_INTERVAL_MS; // sample immediately

    // Reset sensor path so auto-detect runs fresh on each entry.
    // Without this, stale path from AutoRun or a previous session persists.
    s_path = PATH_UNKNOWN;
    s_xyclopsNoTemp = false;

    // Reset 1.6 PIC accumulator — stale values from a previous session
    // would skew the first averaged reading.
    s_avg_amb_acc = 0;
    s_avg_cpu_acc = 0;
    s_avg_count = 0;
}

// ============================================================================
// Draw gauge panel (one per sensor)
// ============================================================================

static void DrawGaugePanel(float px, const char* label,
    int tempC, int warnT, int hotT, bool ok,
    bool showFan = false, int fanPct = 0)
{
    // Panel border
    FillRectGrad(px, GAUGE_Y, px + COL_W, GAUGE_Y + GAUGE_H,
        D3DCOLOR_XRGB(16, 20, 50),
        D3DCOLOR_XRGB(10, 13, 34));
    HLine(GAUGE_Y, px, px + COL_W, COL_CYAN);
    HLine(GAUGE_Y + GAUGE_H, px, px + COL_W, COL_BORDER);
    VLine(px, GAUGE_Y, GAUGE_Y + GAUGE_H, COL_BORDER);
    VLine(px + COL_W, GAUGE_Y, GAUGE_Y + GAUGE_H, COL_BORDER);

    // Section label top-left
    DrawText(px + BIG_X_OFF, GAUGE_Y + 4.f, label, 1.2f, COL_YELLOW);

    if (!ok)
    {
        DrawText(px + BIG_X_OFF, GAUGE_Y + BIG_Y_OFF + 14.f,
            "Please wait...", 1.4f, COL_DIM);
        return;
    }

    DWORD tc = TempColor(tempC, warnT, hotT);

    // Big temperature number
    char numStr[8];
    IntToStr(tempC, numStr, sizeof(numStr));
    StrCat2(numStr, sizeof(numStr), numStr, "C");
    DrawText(px + BIG_X_OFF, GAUGE_Y + BIG_Y_OFF + 14.f,
        numStr, BIG_TEMP_SCALE, tc);

    // Degree symbol approximation: small superscript circle via a dot
    // (font may not have a degree glyph, so we place a small "o" offset up)
    // Skip - the "C" suffix is unambiguous enough

    // Gauge bar background
    float bx = px + BAR_X_OFF;
    float by = GAUGE_Y + BAR_Y_OFF;
    float bw = BAR_W;
    FillRect(bx, by, bx + bw, by + BAR_H, D3DCOLOR_XRGB(14, 17, 38));

    // Filled portion - clamp to 0-GRAPH_MAX
    float fill = (float)tempC / (float)GRAPH_MAX;
    if (fill > 1.f) fill = 1.f;
    if (fill < 0.f) fill = 0.f;

    // Three-zone color: green -> orange -> red gradient fill
    float warnFrac = (float)warnT / GRAPH_MAX;
    float hotFrac = (float)hotT / GRAPH_MAX;
    float fillX = bx + bw * fill;

    if (fill > 0.f)
    {
        if (fill <= warnFrac)
        {
            // All green
            FillRectGrad(bx, by, fillX, by + BAR_H,
                D3DCOLOR_XRGB(40, 200, 60),
                D3DCOLOR_XRGB(20, 120, 30));
        }
        else if (fill <= hotFrac)
        {
            // Green portion + orange portion
            float warnX = bx + bw * warnFrac;
            FillRectGrad(bx, by, warnX, by + BAR_H,
                D3DCOLOR_XRGB(40, 200, 60), D3DCOLOR_XRGB(20, 120, 30));
            FillRectGrad(warnX, by, fillX, by + BAR_H,
                D3DCOLOR_XRGB(220, 160, 20), D3DCOLOR_XRGB(180, 100, 10));
        }
        else
        {
            // Green + orange + red
            float warnX = bx + bw * warnFrac;
            float hotX = bx + bw * hotFrac;
            FillRectGrad(bx, by, warnX, by + BAR_H,
                D3DCOLOR_XRGB(40, 200, 60), D3DCOLOR_XRGB(20, 120, 30));
            FillRectGrad(warnX, by, hotX, by + BAR_H,
                D3DCOLOR_XRGB(220, 160, 20), D3DCOLOR_XRGB(180, 100, 10));
            FillRectGrad(hotX, by, fillX, by + BAR_H,
                D3DCOLOR_XRGB(255, 60, 30), D3DCOLOR_XRGB(180, 20, 10));
        }
    }

    // Threshold tick marks on bar
    float warnX = bx + bw * warnFrac;
    float hotX = bx + bw * hotFrac;
    VLine(warnX, by - 2.f, by + BAR_H + 2.f, COL_ORANGE);
    VLine(hotX, by - 2.f, by + BAR_H + 2.f, COL_RED);

    // Bar border
    HLine(by, bx, bx + bw, COL_BORDER);
    HLine(by + BAR_H, bx, bx + bw, COL_BORDER);
    VLine(bx, by, by + BAR_H, COL_BORDER);
    VLine(bx + bw, by, by + BAR_H, COL_BORDER);

    // Status text
    DrawText(px + BAR_X_OFF, GAUGE_Y + STATUS_Y_OFF,
        TempStatus(tempC, warnT, hotT), 1.2f, tc);

    // Fan speed (CPU panel only, if detected)
    if (showFan && s_fanDetected)
    {
        char fanStr[16];
        StrCopy(fanStr, sizeof(fanStr), "FAN:");
        char pctBuf[8];
        IntToStr(fanPct, pctBuf, sizeof(pctBuf));
        StrCat2(fanStr, sizeof(fanStr), fanStr, pctBuf);
        StrCat2(fanStr, sizeof(fanStr), fanStr, "%");
        // Place to the right of the status label
        DrawText(px + BAR_X_OFF + 60.f, GAUGE_Y + STATUS_Y_OFF,
            fanStr, 1.2f, COL_CYAN);
    }

    // Threshold labels below bar
    char warnLbl[8], hotLbl[8];
    IntToStr(warnT, warnLbl, sizeof(warnLbl)); StrCat2(warnLbl, sizeof(warnLbl), warnLbl, "C");
    IntToStr(hotT, hotLbl, sizeof(hotLbl));  StrCat2(hotLbl, sizeof(hotLbl), hotLbl, "C");
    DrawText(warnX - TW(warnLbl, 0.9f) * 0.5f,
        by + BAR_H + 2.f, warnLbl, 0.9f, COL_ORANGE);
    DrawText(hotX - TW(hotLbl, 0.9f) * 0.5f,
        by + BAR_H + 2.f, hotLbl, 0.9f, COL_RED);
}

// ============================================================================
// Draw history graph
// ============================================================================

static void DrawGraph()
{
    // Background
    FillRect(GRAPH_X, GRAPH_Y, GRAPH_R, GRAPH_B, D3DCOLOR_XRGB(10, 13, 30));

    // Danger zone shading (>HOT threshold)
    float ambHotFrac = (float)(GRAPH_MAX - AMB_HOT) / GRAPH_MAX;
    float cpuHotFrac = (float)(GRAPH_MAX - CPU_HOT) / GRAPH_MAX;
    // Shade top region red (above the higher of the two thresholds = CPU_HOT=80)
    float dangerY = GRAPH_Y + GRAPH_H * cpuHotFrac;
    FillRect(GRAPH_X, GRAPH_Y, GRAPH_R, dangerY,
        D3DCOLOR_XRGB(40, 10, 10));
    // Warm zone (CPU_WARN to CPU_HOT)
    float warnY = GRAPH_Y + GRAPH_H * ((float)(GRAPH_MAX - CPU_WARN) / GRAPH_MAX);
    FillRect(GRAPH_X, dangerY, GRAPH_R, warnY,
        D3DCOLOR_XRGB(28, 18, 8));

    // Horizontal grid lines every 10C
    for (int t = 10; t < GRAPH_MAX; t += 10)
    {
        float gy = GRAPH_B - GRAPH_H * ((float)t / GRAPH_MAX);
        DWORD gc = (t == AMB_HOT || t == CPU_HOT) ? D3DCOLOR_XRGB(100, 30, 30)
            : (t == AMB_WARN || t == CPU_WARN) ? D3DCOLOR_XRGB(80, 60, 20)
            : D3DCOLOR_XRGB(25, 30, 55);
        HLine(gy, GRAPH_X, GRAPH_R, gc);

        // Y axis labels
        char lbl[6]; IntToStr(t, lbl, sizeof(lbl));
        DrawText(GRAPH_X - 24.f, gy - LINE_H * 0.5f, lbl, 0.95f,
            (t == CPU_HOT || t == AMB_HOT) ? COL_RED :
            (t == CPU_WARN || t == AMB_WARN) ? COL_ORANGE : COL_DIM);
    }

    // Border
    HLine(GRAPH_Y, GRAPH_X, GRAPH_R, COL_BORDER);
    HLine(GRAPH_B, GRAPH_X, GRAPH_R, COL_BORDER);
    VLine(GRAPH_X, GRAPH_Y, GRAPH_B, COL_BORDER);
    VLine(GRAPH_R, GRAPH_Y, GRAPH_B, COL_BORDER);

    if (s_histCount < 2) return;

    // Plot lines - newest sample is at right edge
    // Each sample occupies GRAPH_W / (HISTORY_LEN-1) horizontal pixels
    float xStep = GRAPH_W / (float)(HISTORY_LEN - 1);

    for (int i = 0; i < s_histCount - 1; ++i)
    {
        // Position from left: newest is at right so leftmost visible
        // sample index from oldest = (histCount - 1) - (histCount - 1 - i) = i
        // but we want newest at right: sample i from oldest is at
        // x = GRAPH_R - (histCount - 1 - i) * xStep
        int   idxA = HistSample(i);
        int   idxB = HistSample(i + 1);

        float xA = GRAPH_R - (float)(s_histCount - 1 - i) * xStep;
        float xB = GRAPH_R - (float)(s_histCount - 1 - i - 1) * xStep;

        if (xA < GRAPH_X) xA = GRAPH_X;
        if (xB < GRAPH_X) xB = GRAPH_X;

        // Ambient line (cyan)
        float ayA = GRAPH_B - GRAPH_H * ((float)s_histAmb[idxA] / GRAPH_MAX);
        float ayB = GRAPH_B - GRAPH_H * ((float)s_histAmb[idxB] / GRAPH_MAX);
        // Simple line via stepping along x
        {
            int steps = Ftoi(xB - xA);
            if (steps < 1) steps = 1;
            for (int s = 0; s <= steps; ++s)
            {
                float t2 = (float)s / steps;
                float lx = xA + (xB - xA) * t2;
                float ly = ayA + (ayB - ayA) * t2;
                if (lx >= GRAPH_X && lx <= GRAPH_R &&
                    ly >= GRAPH_Y && ly <= GRAPH_B)
                {
                    FillRect(lx, ly - 1.f, lx + 2.f, ly + 1.f, COL_CYAN);
                }
            }
        }

        // CPU line (yellow)
        float cyA = GRAPH_B - GRAPH_H * ((float)s_histCPU[idxA] / GRAPH_MAX);
        float cyB = GRAPH_B - GRAPH_H * ((float)s_histCPU[idxB] / GRAPH_MAX);
        {
            int steps = Ftoi(xB - xA);
            if (steps < 1) steps = 1;
            for (int s = 0; s <= steps; ++s)
            {
                float t2 = (float)s / steps;
                float lx = xA + (xB - xA) * t2;
                float ly = cyA + (cyB - cyA) * t2;
                if (lx >= GRAPH_X && lx <= GRAPH_R &&
                    ly >= GRAPH_Y && ly <= GRAPH_B)
                {
                    FillRect(lx - 1.f, ly - 1.f, lx + 1.f, ly + 1.f, COL_YELLOW);
                }
            }
        }
    }

    // Latest value dots (larger) at right edge
    if (s_histCount > 0 && s_sensorOK)
    {
        float dotX = GRAPH_R - 3.f;
        float ambY = GRAPH_B - GRAPH_H * ((float)s_curAmb / GRAPH_MAX);
        float cpuY = GRAPH_B - GRAPH_H * ((float)s_curCPU / GRAPH_MAX);
        FillRect(dotX - 3.f, ambY - 3.f, dotX + 3.f, ambY + 3.f, COL_CYAN);
        FillRect(dotX - 3.f, cpuY - 3.f, dotX + 3.f, cpuY + 3.f, COL_YELLOW);
    }

    // Graph legend (top-right inside graph)
    float lx = GRAPH_R - 130.f;
    float ly = GRAPH_Y + 5.f;
    FillRect(lx, ly, lx + 10.f, ly + 6.f, COL_CYAN);
    DrawText(lx + 13.f, ly - 2.f, "AMBIENT", 1.0f, COL_CYAN);
    ly += 12.f;
    FillRect(lx, ly, lx + 10.f, ly + 6.f, COL_YELLOW);
    DrawText(lx + 13.f, ly - 2.f,
        (s_path == PATH_PIC_16) ? "CPU TEMP" : "CPU DIE", 1.0f, COL_YELLOW);

    // Sample count / time scale
    char scaleStr[32];
    StrCopy(scaleStr, sizeof(scaleStr), "~");
    char t2[8];
    IntToStr(s_histCount / 2, t2, sizeof(t2));
    StrCat2(scaleStr, sizeof(scaleStr), scaleStr, t2);
    StrCat2(scaleStr, sizeof(scaleStr), scaleStr, "s history");
    DrawText(GRAPH_X + 4.f, GRAPH_Y + 4.f, scaleStr, 1.0f, COL_DIM);
}

// ============================================================================
// Tick
// ============================================================================

void TempMonitor_Tick(const DiagLogo& logo)
{
    WORD cur = GetButtons();

    if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
    {
        RequestState(MSTATE_MENU);
        s_prevBtns = cur;
        return;
    }
    s_prevBtns = cur;

    // Sample ADM1032 on interval
    DWORD now = GetTickCount();
    if ((now - s_lastSample) >= SAMPLE_INTERVAL_MS)
    {
        TakeSample();
        s_lastSample = now;
    }

    // Render
    g_pDevice->BeginScene();

    // Build hint with refresh indicator (flickers on each sample)
    const char* hint = "[B] Back";
    DrawPageChrome(logo, "TEMP MONITOR", hint);

    // Divider label between gauges
    float midX = (COL_L_X + COL_W + COL_R_X) * 0.5f;
    float midY = GAUGE_Y + GAUGE_H * 0.5f - LINE_H * 0.5f;
    {
        const char* sLabel = (s_path == PATH_ADM1032) ? "ADM1032" :
            (s_path == PATH_PIC_16 && s_xyclopsNoTemp) ? "XYCLOPS" :
            (s_path == PATH_PIC_16) ? "PIC/XCAL" :
            "SENSOR";
        DrawText(midX - TW(sLabel, 1.0f) * 0.5f, midY, sLabel, 1.0f, COL_DIM);
    }

    DrawGaugePanel(COL_L_X, "AMBIENT (BOARD)", (int)s_curAmb,
        AMB_WARN, AMB_HOT, s_sensorOK && !s_xyclopsNoTemp);
    DrawGaugePanel(COL_R_X,
        (s_path == PATH_PIC_16) ? "CPU TEMP" : "CPU DIE",
        (int)s_curCPU, CPU_WARN, CPU_HOT, s_sensorOK && !s_xyclopsNoTemp,
        true, (int)s_curFan * 2);

    // Graph section header
    HLine(GRAPH_Y - 14.f, LM, SW - LM, COL_BORDER);
    DrawText(LM, GRAPH_Y - 13.f, "TEMPERATURE HISTORY", 1.1f, COL_YELLOW);

    // Sensor status overlay
    if (s_xyclopsNoTemp)
    {
        // Xyclops (1.6 SMC) does not expose temp registers at 0x09/0x0A.
        // Show informational message rather than a scary error.
        DrawText(LM, GAUGE_Y + 20.f,
            "N/A  -  Xyclops SMC detected", 1.3f, COL_GRAY);
        DrawText(LM, GAUGE_Y + 38.f,
            "Temperature registers (0x09/0x0A) not implemented", 1.15f, COL_DIM);
        DrawText(LM, GAUGE_Y + 54.f,
            "on 1.6 Xyclops hardware via this interface.", 1.15f, COL_DIM);
    }
    else if (!s_sensorOK)
    {
        // Sensor path not yet confirmed or accumulator still filling —
        // show a friendly wait message centred in the graph area where
        // it is clearly visible rather than buried in the gauge panels.
        const char* waitMsg = (s_path == PATH_UNKNOWN)
            ? "Please wait — reading sensor..."
            : "Please wait — reading temperature data...";
        float msgY = GRAPH_Y + GRAPH_H * 0.5f - LINE_H * 0.5f;
        float msgX = GRAPH_X + (GRAPH_W - TW(waitMsg, 1.3f)) * 0.5f;
        DrawText(msgX, msgY, waitMsg, 1.3f, COL_DIM);
    }

    DrawGraph();

    // Bottom hint — show sensor address and poll rate.
    // Right-aligned left of the badge so they don't overlap.
    // TW(g_videoModeStr) accounts for badge width at current advance.
    {
        const char* pollHint = (s_path == PATH_PIC_16 && s_xyclopsNoTemp)
            ? "Xyclops 1.6 SMC  -  temp N/A"
            : (s_path == PATH_PIC_16)
            ? "500ms poll  PIC 0x20 (10-sample avg)"
            : "500ms poll  ADM1032 0x98";
        float badgeW = TW(g_videoModeStr, 1.3f);
        float hintRX = SW - LM - badgeW - 10.f;
        float hintY = BOT_BAR_Y + (BOT_BAR_H - LINE_H) * 0.5f;
        DrawTextR(hintRX, hintY, pollHint, 1.05f, COL_DIM);
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}
// ============================================================================
// AutoRun — headless temperature sampling for XbSet automation
// Uses the same TakeSample() path as the interactive module.
// Waits up to 2s for path detection to settle, then takes 5 samples.
// ============================================================================

void TempMonitor_AutoRun(HANDLE hReport)
{
    // Reset so TakeSample() re-detects the path fresh
    s_path = PATH_UNKNOWN;
    s_sensorOK = false;
    s_fanDetected = false;
    s_curCPU = 0; s_curAmb = 0; s_curFan = 0;

    char line[128]; DWORD w;
    auto WL = [&](const char* lbl, const char* val)
        {
            StrCopy(line, sizeof(line), lbl);
            StrCat2(line, sizeof(line), line, val);
            StrCat2(line, sizeof(line), line, "\r\n");
            WriteFile(hReport, line, StrLen(line), &w, NULL);
        };

    // Warmup: detect path then prime the accumulator.
    // PATH_ADM1032: one call detects path and reads temps immediately.
    // PATH_PIC_16:  requires AVG_SAMPLES (10) successful TakeSample() calls
    //               before s_sensorOK goes true — the accumulator must fill
    //               before any valid reading is committed.
    // Run up to AVG_SAMPLES * 3 iterations so the PIC path always completes
    // at least one full accumulation cycle before measurement begins.
    {
        int i;
        for (i = 0; i < AVG_SAMPLES * 3; ++i)
        {
            TakeSample();
            Sleep(200);
            // ADM1032: path known and sensorOK after first successful read
            if (s_path == PATH_ADM1032 && s_sensorOK) break;
            // PIC_16: wait until accumulator has committed at least once
            if (s_path == PATH_PIC_16 && s_avg_count == 0 && s_sensorOK) break;
        }
    }

    if (s_path == PATH_UNKNOWN)
    {
        WL("Sensor:       ", "No response - path not detected after 2s");
        return;
    }

    WL("Sensor path:  ", s_path == PATH_ADM1032
        ? "ADM1032 (rev 1.0-1.5)" : "PIC proxy (rev 1.6)");

    // Take 5 measurement samples at 500ms intervals
    int sumCPU = 0, sumAmb = 0, sumFan = 0;
    int goodSamples = 0, fanSamples = 0;

    for (int i = 0; i < 5; ++i)
    {
        TakeSample();
        if (s_sensorOK)
        {
            sumCPU += (int)s_curCPU;
            sumAmb += (int)s_curAmb;
            if (s_fanDetected) { sumFan += (int)s_curFan * 2; ++fanSamples; }
            ++goodSamples;
        }
        Sleep(500);
    }

    if (goodSamples > 0)
    {
        char t[12];
        IntToStr(sumCPU / goodSamples, t, sizeof(t));
        StrCat2(t, sizeof(t), t, " C");  WL("CPU Die avg:  ", t);

        IntToStr(sumAmb / goodSamples, t, sizeof(t));
        StrCat2(t, sizeof(t), t, " C");  WL("Ambient avg:  ", t);

        if (fanSamples > 0)
        {
            IntToStr(sumFan / fanSamples, t, sizeof(t));
            StrCat2(t, sizeof(t), t, "%"); WL("Fan Speed avg:", t);
        }
        else
        {
            WL("Fan Speed:    ", "Not detected");
        }
    }
    else
    {
        WL("Sensor:       ", "Path found but no valid readings");
    }
}