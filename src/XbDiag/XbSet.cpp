// XbSet.cpp
// XbDiag automation settings editor and autorun engine.
//
// Settings file format (D:\XbDiag.set):
//   Module enables (0/1): SYSINFO  VIDEOINFO  SMBUS  HDDINFO  RAMTEST
//                         TEMPMON  EEPROM  CTRLTEST  CPUSTRESS  RAMSTRESS
//   CPU_HRS=N  CPU_MIN=N  RAM_HRS=N  RAM_MIN=N  LOOPS=N
//   ALT_STRESS=0/1  SHUTDOWN=0/1
//   Unrecognised keys are ignored. CRLF or LF accepted.
//
// Autorun sequence:
//   SysInfo -> VideoInfo -> SmBusScan -> HddInfo -> RamTest -> TempMon
//   -> EepromView -> ControllerTest (snapshot) -> Stress loop(s)
//
// Stress loop modes:
//   Each loop runs: CPU stress (if enabled) then RAM stress (if enabled).
//   Repeat stressLoops times. ALT_STRESS changes the report label only.
//   Per-test durations: cpuStress / ramStress (independent).
//
// Shutdown: PIC16L power-off command (matches PrometheOS utils::shutdown).

#include "XbSet.h"
#include "font.h"
#include "input.h"
#include "DiagCommon.h"
#include "SysInfo.h"
#include "VideoInfo.h"
#include "SmBusScan.h"
#include "HddInfo.h"
#include "RamTest.h"
#include "TempMonitor.h"
#include "EepromView.h"
#include "ControllerTest.h"
#include "StressTest.h"
#include "StressTestCPU.h"
#include "StressMath.h"
#include "StressTestGPU.h"
extern void StressTest_AutoRun(HANDLE hReport, DWORD durationMs);
extern void RamStress_AutoRun(HANDLE hReport, DWORD durationMs);

// Per-loop result accessors (implemented in StressTest.cpp)
extern BYTE  StressAutoRun_GetMinCPU();
extern BYTE  StressAutoRun_GetMaxCPU();
extern BYTE  StressAutoRun_GetMinFan();
extern BYTE  StressAutoRun_GetMaxFan();
extern BYTE  StressAutoRun_GetMeasuredLoad();
extern bool  StressAutoRun_GetThermalAbort();
extern DWORD RamAutoRun_GetSweeps();
extern DWORD RamAutoRun_GetErrors();
extern int   RamAutoRun_GetFailed();
#include <xtl.h>
#include <winsockx.h>

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

// Xbox shutdown via PIC16L power command — matches PrometheOS utils::shutdown()
// PIC16L at SMBus 0x20, command 0x02 (power), value 0x80 (power off subcmd)
static void XbShutdown()
{
    SMBusWrite(SMBADDR_PIC, 0x02, 0x80);
}

// ============================================================================
// Globals
// ============================================================================

AutoSettings g_autoSettings;
bool         g_autoSettingsFound = false;

#define SETTINGS_PATH  "D:\\XbDiag.set"
#define REPORT_PATH    "D:\\XbDiag.txt"

// ============================================================================
// String utilities
// ============================================================================

static bool StrEq(const char* a, const char* b)
{
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == '\0' && *b == '\0';
}

static int ParseInt(const char* val, int lo, int hi, int fallback)
{
    int v = 0;
    for (int i = 0; val[i] >= '0' && val[i] <= '9'; ++i) v = v * 10 + (val[i] - '0');
    return (v >= lo && v <= hi) ? v : fallback;
}

// ============================================================================
// Settings I/O
// ============================================================================

bool XbSet_LoadSettings()
{
    // Defaults
    g_autoSettings.runSysInfo = false;
    g_autoSettings.runVideoInfo = false;
    g_autoSettings.runSmBus = false;
    g_autoSettings.runHddInfo = false;
    g_autoSettings.runRamTest = false;
    g_autoSettings.runTempMon = false;
    g_autoSettings.runEeprom = false;
    g_autoSettings.runCtrlTest = false;
    g_autoSettings.runCpuStress = false;
    g_autoSettings.runRamStress = false;
    g_autoSettings.runGpuStress = false;
    g_autoSettings.cpuStressHours = 0;
    g_autoSettings.cpuStressMins = 30;
    g_autoSettings.ramStressHours = 0;
    g_autoSettings.ramStressMins = 30;
    g_autoSettings.gpuStressHours = 0;
    g_autoSettings.gpuStressMins = 30;
    g_autoSettings.stressLoops = 1;
    g_autoSettings.altStressMode = false;
    g_autoSettings.shutdownAfter = false;

    HANDLE hf = CreateFileA(SETTINGS_PATH, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;

    char buf[4096]; DWORD nr = 0;
    ReadFile(hf, buf, sizeof(buf) - 1, &nr, NULL);
    CloseHandle(hf);
    buf[nr] = '\0';

    char* p = buf;
    while (*p)
    {
        char* ls = p;
        while (*p && *p != '\r' && *p != '\n') ++p;
        char* le = p;
        while (*p == '\r' || *p == '\n') ++p;
        *le = '\0';

        char* eq = ls;
        while (*eq && *eq != '=') ++eq;
        if (!*eq) continue;
        *eq = '\0';
        const char* key = ls;
        const char* val = eq + 1;
        bool on = (val[0] == '1');

        if (StrEq(key, "SYSINFO"))      g_autoSettings.runSysInfo = on;
        if (StrEq(key, "VIDEOINFO"))    g_autoSettings.runVideoInfo = on;
        if (StrEq(key, "SMBUS"))        g_autoSettings.runSmBus = on;
        if (StrEq(key, "HDDINFO"))      g_autoSettings.runHddInfo = on;
        if (StrEq(key, "RAMTEST"))      g_autoSettings.runRamTest = on;
        if (StrEq(key, "TEMPMON"))      g_autoSettings.runTempMon = on;
        if (StrEq(key, "EEPROM"))       g_autoSettings.runEeprom = on;
        if (StrEq(key, "CTRLTEST"))     g_autoSettings.runCtrlTest = on;
        if (StrEq(key, "CPUSTRESS"))    g_autoSettings.runCpuStress = on;
        if (StrEq(key, "RAMSTRESS"))    g_autoSettings.runRamStress = on;
        if (StrEq(key, "GPUSTRESS"))    g_autoSettings.runGpuStress = on;
        if (StrEq(key, "CPU_HRS"))      g_autoSettings.cpuStressHours = ParseInt(val, 0, 99, 0);
        if (StrEq(key, "CPU_MIN"))      g_autoSettings.cpuStressMins = ParseInt(val, 0, 59, 30);
        if (StrEq(key, "RAM_HRS"))      g_autoSettings.ramStressHours = ParseInt(val, 0, 99, 0);
        if (StrEq(key, "RAM_MIN"))      g_autoSettings.ramStressMins = ParseInt(val, 0, 59, 30);
        if (StrEq(key, "GPU_HRS"))      g_autoSettings.gpuStressHours = ParseInt(val, 0, 99, 0);
        if (StrEq(key, "GPU_MIN"))      g_autoSettings.gpuStressMins = ParseInt(val, 0, 59, 30);
        if (StrEq(key, "LOOPS"))        g_autoSettings.stressLoops = ParseInt(val, 1, 99, 1);
        if (StrEq(key, "ALT_STRESS"))   g_autoSettings.altStressMode = on;
        if (StrEq(key, "SHUTDOWN"))     g_autoSettings.shutdownAfter = on;
    }
    return true;
}

bool XbSet_SaveSettings()
{
    HANDLE hf = CreateFileA(SETTINGS_PATH, GENERIC_WRITE, 0,
        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;

    char line[64]; DWORD w;
    const char* hdr = "# XbDiag automation settings\r\n# 1=enabled 0=disabled\r\n";
    WriteFile(hf, hdr, StrLen(hdr), &w, NULL);

    auto WL = [&](const char* key, bool val)
        {
            StrCopy(line, sizeof(line), key);
            StrCat2(line, sizeof(line), line, val ? "=1\r\n" : "=0\r\n");
            WriteFile(hf, line, StrLen(line), &w, NULL);
        };
    auto WN = [&](const char* key, int val)
        {
            StrCopy(line, sizeof(line), key);
            StrCat2(line, sizeof(line), line, "=");
            char v[8]; IntToStr(val, v, sizeof(v));
            StrCat2(line, sizeof(line), line, v);
            StrCat2(line, sizeof(line), line, "\r\n");
            WriteFile(hf, line, StrLen(line), &w, NULL);
        };

    WL("SYSINFO", g_autoSettings.runSysInfo);
    WL("VIDEOINFO", g_autoSettings.runVideoInfo);
    WL("SMBUS", g_autoSettings.runSmBus);
    WL("HDDINFO", g_autoSettings.runHddInfo);
    WL("RAMTEST", g_autoSettings.runRamTest);
    WL("TEMPMON", g_autoSettings.runTempMon);
    WL("EEPROM", g_autoSettings.runEeprom);
    WL("CTRLTEST", g_autoSettings.runCtrlTest);
    WL("CPUSTRESS", g_autoSettings.runCpuStress);
    WL("RAMSTRESS", g_autoSettings.runRamStress);
    WL("GPUSTRESS", g_autoSettings.runGpuStress);
    WN("CPU_HRS", g_autoSettings.cpuStressHours);
    WN("CPU_MIN", g_autoSettings.cpuStressMins);
    WN("RAM_HRS", g_autoSettings.ramStressHours);
    WN("RAM_MIN", g_autoSettings.ramStressMins);
    WN("GPU_HRS", g_autoSettings.gpuStressHours);
    WN("GPU_MIN", g_autoSettings.gpuStressMins);
    WN("LOOPS", g_autoSettings.stressLoops);
    WL("ALT_STRESS", g_autoSettings.altStressMode);
    WL("SHUTDOWN", g_autoSettings.shutdownAfter);

    FlushFileBuffers(hf);
    CloseHandle(hf);
    return true;
}

bool XbSet_DeleteSettings()
{
    if (DeleteFileA(SETTINGS_PATH))
    {
        g_autoSettingsFound = false;
        return true;
    }
    return false;
}

// ============================================================================
// Report helpers
// ============================================================================

static void RW(HANDLE hf, const char* label, const char* val)
{
    char line[256]; DWORD w;
    StrCopy(line, sizeof(line), label);
    StrCat2(line, sizeof(line), line, val);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
}

static void RSection(HANDLE hf, const char* name)
{
    DWORD w;
    const char* sep = "------------------------------------------------------------\r\n";
    WriteFile(hf, sep, StrLen(sep), &w, NULL);
    char line[80];
    StrCopy(line, sizeof(line), name);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);
    WriteFile(hf, sep, StrLen(sep), &w, NULL);
}

// ============================================================================
// AutoRun function declarations (implemented in each module's .cpp)
// ============================================================================

extern void SysInfo_AutoRun(HANDLE hReport);
extern void VideoInfo_AutoRun(HANDLE hReport);
extern void SmBusScan_AutoRun(HANDLE hReport);
extern void HddInfo_AutoRun(HANDLE hReport);
extern void RamTest_AutoRun(HANDLE hReport);
extern void TempMonitor_AutoRun(HANDLE hReport);
extern void EepromView_AutoRun(HANDLE hReport);
extern void ControllerTest_AutoRun(HANDLE hReport);

// ============================================================================
// Duration label helper
// ============================================================================

static void FmtDurLabel(int hours, int mins, char* buf, int bufLen)
{
    if (hours > 0)
    {
        char hh[8], mm[8];
        IntToStr(hours, hh, sizeof(hh));
        IntToStr(mins, mm, sizeof(mm));
        StrCopy(buf, bufLen, hh);
        StrCat2(buf, bufLen, buf, "h ");
        StrCat2(buf, bufLen, buf, mm);
        StrCat2(buf, bufLen, buf, "m");
    }
    else
    {
        char mm[8]; IntToStr(mins, mm, sizeof(mm));
        StrCopy(buf, bufLen, mm);
        StrCat2(buf, bufLen, buf, "m");
    }
}

static DWORD DurToMs(int hours, int mins)
{
    DWORD secs = (DWORD)(hours * 3600 + mins * 60);
    if (secs < 60) secs = 60;  // enforce 1 minute minimum
    return secs * 1000;
}

// ============================================================================
// Status overlay helpers
// ============================================================================

static void DrawAutoStatus(const DiagLogo& logo,
    const char* moduleName, int step, int total, const char* subMsg = NULL)
{
    g_pDevice->BeginScene();
    DrawPageChrome(logo, "AUTO DIAGNOSTIC", "Running...");

    float py = CONTENT_Y + 30.f;
    DrawText(LM, py, "AUTOMATED DIAGNOSTICS IN PROGRESS", 1.4f, COL_YELLOW);
    py += LINE_H + 8.f;

    char stepBuf[32];
    char sa[8], sb[8];
    IntToStr(step, sa, sizeof(sa)); IntToStr(total, sb, sizeof(sb));
    StrCopy(stepBuf, sizeof(stepBuf), "Step ");
    StrCat2(stepBuf, sizeof(stepBuf), stepBuf, sa);
    StrCat2(stepBuf, sizeof(stepBuf), stepBuf, " of ");
    StrCat2(stepBuf, sizeof(stepBuf), stepBuf, sb);
    DrawText(LM, py, stepBuf, 1.2f, COL_GRAY);
    py += LINE_H + 2.f;

    DrawText(LM, py, moduleName, 1.7f, COL_CYAN);
    py += LINE_H + 10.f;

    if (subMsg)
    {
        DrawText(LM, py, subMsg, 1.2f, COL_ORANGE);
        py += LINE_H + 4.f;
    }

    float bw = SW - LM * 2.f;
    float frac = (total > 1) ? (float)(step - 1) / (float)(total - 1) : 0.f;
    if (frac > 1.f) frac = 1.f;
    FillRect(LM, py, LM + bw, py + 10.f, D3DCOLOR_XRGB(14, 17, 38));
    if (frac > 0.f)
        FillRectGrad(LM, py, LM + bw * frac, py + 10.f,
            D3DCOLOR_XRGB(60, 200, 80), D3DCOLOR_XRGB(20, 120, 40));
    HLine(py, LM, LM + bw, COL_BORDER);
    HLine(py + 10.f, LM, LM + bw, COL_BORDER);
    py += 18.f;
    DrawText(LM, py, "Results -> XbDiag.txt", 1.0f, COL_DIM);

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

static void DrawAutoComplete(const DiagLogo& logo, bool ok, bool willShutdown)
{
    g_pDevice->BeginScene();
    DrawPageChrome(logo, "AUTO DIAGNOSTIC", "[A] Return to menu");
    float py = CONTENT_Y + 50.f;
    if (ok)
    {
        DrawText(LM, py, "DIAGNOSTICS COMPLETE", 1.6f, COL_GREEN); py += LINE_H + 8.f;
        DrawText(LM, py, "Results saved to D:\\XbDiag.txt", 1.2f, COL_CYAN);
    }
    else
    {
        DrawText(LM, py, "DIAGNOSTICS COMPLETE", 1.6f, COL_ORANGE); py += LINE_H + 8.f;
        DrawText(LM, py, "Could not write D:\\XbDiag.txt", 1.2f, COL_RED);
    }
    py += LINE_H * 2.f;
    DrawText(LM, py, "Press [A] or [B] to continue.", 1.2f, COL_GRAY);
    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// ============================================================================
// AutoRun engine
// ============================================================================

void XbSet_AutoRun(const DiagLogo& logo)
{
    const int loops = (g_autoSettings.stressLoops > 0) ? g_autoSettings.stressLoops : 1;
    const bool doAlt = g_autoSettings.altStressMode;
    const bool doCpu = g_autoSettings.runCpuStress;
    const bool doRam = g_autoSettings.runRamStress;
    const bool doGpu = g_autoSettings.runGpuStress;

    // Total steps: non-stress modules run once, stress modules run once per loop
    int stressStepsPerLoop = (doCpu ? 1 : 0) + (doRam ? 1 : 0) + (doGpu ? 1 : 0);
    int total = 0;
    if (g_autoSettings.runSysInfo)   ++total;
    if (g_autoSettings.runVideoInfo) ++total;
    if (g_autoSettings.runSmBus)     ++total;
    if (g_autoSettings.runHddInfo)   ++total;
    if (g_autoSettings.runRamTest)   ++total;
    if (g_autoSettings.runTempMon)   ++total;
    if (g_autoSettings.runEeprom)    ++total;
    if (g_autoSettings.runCtrlTest)  ++total;
    total += stressStepsPerLoop * loops;

    if (total == 0) { RequestState(MSTATE_MENU); return; }

    // Per-stress duration in ms
    DWORD cpuMs = DurToMs(g_autoSettings.cpuStressHours, g_autoSettings.cpuStressMins);
    DWORD ramMs = DurToMs(g_autoSettings.ramStressHours, g_autoSettings.ramStressMins);
    DWORD gpuMs = DurToMs(g_autoSettings.gpuStressHours, g_autoSettings.gpuStressMins);

    char cpuLabel[16], ramLabel[16], gpuLabel[16];
    FmtDurLabel(g_autoSettings.cpuStressHours, g_autoSettings.cpuStressMins, cpuLabel, sizeof(cpuLabel));
    FmtDurLabel(g_autoSettings.ramStressHours, g_autoSettings.ramStressMins, ramLabel, sizeof(ramLabel));
    FmtDurLabel(g_autoSettings.gpuStressHours, g_autoSettings.gpuStressMins, gpuLabel, sizeof(gpuLabel));

    HANDLE hf = CreateFileA(REPORT_PATH, GENERIC_WRITE, 0,
        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    bool reportOK = (hf != INVALID_HANDLE_VALUE);

    if (reportOK)
    {
        DWORD w;
        const char* hdr =
            "XbDiag Automated Diagnostic Report\r\n"
            "====================================\r\n\r\n";
        WriteFile(hf, hdr, StrLen(hdr), &w, NULL);

        // Log stress configuration
        char line[80];
        StrCopy(line, sizeof(line), "CPU stress duration: ");
        StrCat2(line, sizeof(line), line, cpuLabel);
        StrCat2(line, sizeof(line), line, "\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);

        StrCopy(line, sizeof(line), "RAM stress duration: ");
        StrCat2(line, sizeof(line), line, ramLabel);
        StrCat2(line, sizeof(line), line, "\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);

        StrCopy(line, sizeof(line), "GPU stress duration: ");
        StrCat2(line, sizeof(line), line, gpuLabel);
        StrCat2(line, sizeof(line), line, "\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);

        char lb[8]; IntToStr(loops, lb, sizeof(lb));
        StrCopy(line, sizeof(line), "Stress loops:        ");
        StrCat2(line, sizeof(line), line, lb);
        StrCat2(line, sizeof(line), line, "\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);

        StrCopy(line, sizeof(line), "Stress mode:         ");
        StrCat2(line, sizeof(line), line, doAlt ? "Alternate (CPU/RAM interleaved)\r\n" : "Normal (CPU then RAM)\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);

        WriteFile(hf, "\r\n", 2, &w, NULL);
    }

    int step = 1;

    // ---- Non-stress modules (run once) ----

    if (g_autoSettings.runSysInfo)
    {
        DrawAutoStatus(logo, "SYSTEM INFO", step++, total);
        SysInfo_OnEnter();
        if (reportOK) { RSection(hf, "SYSTEM INFO"); SysInfo_AutoRun(hf); DWORD _w; WriteFile(hf, "\r\n", 2, &_w, NULL); }
    }
    if (g_autoSettings.runVideoInfo)
    {
        DrawAutoStatus(logo, "VIDEO INFO", step++, total);
        VideoInfo_OnEnter();
        if (reportOK) { RSection(hf, "VIDEO INFO"); VideoInfo_AutoRun(hf); DWORD _w; WriteFile(hf, "\r\n", 2, &_w, NULL); }
    }
    if (g_autoSettings.runSmBus)
    {
        DrawAutoStatus(logo, "SMBUS SCAN", step++, total);
        SmBusScan_OnEnter();
        if (reportOK) { RSection(hf, "SMBUS SCAN"); SmBusScan_AutoRun(hf); DWORD _w; WriteFile(hf, "\r\n", 2, &_w, NULL); }
    }
    if (g_autoSettings.runHddInfo)
    {
        DrawAutoStatus(logo, "HDD INFO + BENCHMARK", step++, total);
        HddInfo_OnEnter();
        if (reportOK) { RSection(hf, "HDD INFO"); HddInfo_AutoRun(hf); DWORD _w; WriteFile(hf, "\r\n", 2, &_w, NULL); }
    }
    if (g_autoSettings.runRamTest)
    {
        DrawAutoStatus(logo, "RAM TEST", step++, total);
        RamTest_OnEnter();
        if (reportOK) { RSection(hf, "RAM TEST"); RamTest_AutoRun(hf); DWORD _w; WriteFile(hf, "\r\n", 2, &_w, NULL); }
    }
    if (g_autoSettings.runTempMon)
    {
        DrawAutoStatus(logo, "TEMP MONITOR", step++, total);
        TempMonitor_OnEnter();
        if (reportOK) { RSection(hf, "TEMP MONITOR"); TempMonitor_AutoRun(hf); DWORD _w; WriteFile(hf, "\r\n", 2, &_w, NULL); }
    }
    if (g_autoSettings.runEeprom)
    {
        DrawAutoStatus(logo, "EEPROM", step++, total);
        EepromView_OnEnter();
        if (reportOK) { RSection(hf, "EEPROM"); EepromView_AutoRun(hf); DWORD _w; WriteFile(hf, "\r\n", 2, &_w, NULL); }
    }
    if (g_autoSettings.runCtrlTest)
    {
        DrawAutoStatus(logo, "CONTROLLER TEST", step++, total);
        if (reportOK) { RSection(hf, "CONTROLLER TEST"); ControllerTest_AutoRun(hf); DWORD _w; WriteFile(hf, "\r\n", 2, &_w, NULL); }
        else            ControllerTest_AutoRun(INVALID_HANDLE_VALUE);
    }

    // ---- Stress loops ----

    // Accumulators for cross-loop summary (only used when loops > 1)
    BYTE  acc_cpuMin = 255, acc_cpuMax = 0;
    BYTE  acc_fanMin = 255, acc_fanMax = 0;
    DWORD acc_cpuPeakSum = 0;
    int   acc_cpuLoops = 0;
    bool  acc_thermalAny = false;
    DWORD acc_ramSweeps = 0;
    DWORD acc_ramErrors = 0;
    bool  acc_ramFailed = false;
    int   acc_ramLoops = 0;
    DWORD acc_gpuLoops = 0;
    float acc_gpuPeak = 0.f;
    float acc_gpuMin = 9999.f;
    int   acc_gpuRuns = 0;

    for (int loop = 0; loop < loops; ++loop)
    {
        // Build loop label for report and status
        char loopLabel[32];
        if (loops > 1)
        {
            char la[8], lb[8];
            IntToStr(loop + 1, la, sizeof(la));
            IntToStr(loops, lb, sizeof(lb));
            StrCopy(loopLabel, sizeof(loopLabel), "Loop ");
            StrCat2(loopLabel, sizeof(loopLabel), loopLabel, la);
            StrCat2(loopLabel, sizeof(loopLabel), loopLabel, " / ");
            StrCat2(loopLabel, sizeof(loopLabel), loopLabel, lb);
        }
        else loopLabel[0] = '\0';

        if (doCpu)
        {
            char statusLabel[48];
            StrCopy(statusLabel, sizeof(statusLabel), "CPU STRESS");
            if (loopLabel[0]) { StrCat2(statusLabel, sizeof(statusLabel), statusLabel, "  "); StrCat2(statusLabel, sizeof(statusLabel), statusLabel, loopLabel); }

            char subMsg[32];
            StrCopy(subMsg, sizeof(subMsg), cpuLabel);
            if (loops > 1) { StrCat2(subMsg, sizeof(subMsg), subMsg, "  "); StrCat2(subMsg, sizeof(subMsg), subMsg, loopLabel); }

            DrawAutoStatus(logo, statusLabel, step, total, subMsg);

            // AutoRun defaults — no wizard ran so set these explicitly.
            // 90C is the safe ceiling for unattended runs; fan stays under SMC.
            s_tempThreshold = 90;
            ST_CPU_FanRelease();

            if (reportOK)
            {
                char secName[48];
                StrCopy(secName, sizeof(secName), "CPU STRESS TEST");
                if (loopLabel[0]) { StrCat2(secName, sizeof(secName), secName, " - "); StrCat2(secName, sizeof(secName), secName, loopLabel); }
                RSection(hf, secName);
                StressTest_AutoRun(hf, cpuMs);
                DWORD _w; WriteFile(hf, "\r\n", 2, &_w, NULL);
            }
            else
            {
                StressTest_AutoRun(INVALID_HANDLE_VALUE, cpuMs);
            }

            // Accumulate per-loop CPU results
            BYTE loopMin = StressAutoRun_GetMinCPU();
            BYTE loopMax = StressAutoRun_GetMaxCPU();
            BYTE loopFanMin = StressAutoRun_GetMinFan();
            BYTE loopFanMax = StressAutoRun_GetMaxFan();
            if (loopMin < acc_cpuMin) acc_cpuMin = loopMin;
            if (loopMax > acc_cpuMax) acc_cpuMax = loopMax;
            if (loopFanMin < acc_fanMin) acc_fanMin = loopFanMin;
            if (loopFanMax > acc_fanMax) acc_fanMax = loopFanMax;
            acc_cpuPeakSum += loopMax;
            ++acc_cpuLoops;
            if (StressAutoRun_GetThermalAbort()) acc_thermalAny = true;

            ++step;
        }

        if (doRam)
        {
            char statusLabel[48];
            StrCopy(statusLabel, sizeof(statusLabel), "RAM STRESS");
            if (loopLabel[0]) { StrCat2(statusLabel, sizeof(statusLabel), statusLabel, "  "); StrCat2(statusLabel, sizeof(statusLabel), statusLabel, loopLabel); }

            char subMsg[32];
            StrCopy(subMsg, sizeof(subMsg), ramLabel);
            if (loops > 1) { StrCat2(subMsg, sizeof(subMsg), subMsg, "  "); StrCat2(subMsg, sizeof(subMsg), subMsg, loopLabel); }

            DrawAutoStatus(logo, statusLabel, step, total, subMsg);

            if (reportOK)
            {
                char secName[48];
                StrCopy(secName, sizeof(secName), "RAM STRESS TEST");
                if (loopLabel[0]) { StrCat2(secName, sizeof(secName), secName, " - "); StrCat2(secName, sizeof(secName), secName, loopLabel); }
                RSection(hf, secName);
                RamStress_AutoRun(hf, ramMs);
                DWORD _w; WriteFile(hf, "\r\n", 2, &_w, NULL);
            }
            else
            {
                RamStress_AutoRun(INVALID_HANDLE_VALUE, ramMs);
            }

            // Accumulate per-loop RAM results
            acc_ramSweeps += RamAutoRun_GetSweeps();
            acc_ramErrors += RamAutoRun_GetErrors();
            if (RamAutoRun_GetFailed() > 0) acc_ramFailed = true;
            ++acc_ramLoops;

            ++step;
        }

        if (doGpu)
        {
            char statusLabel[48];
            StrCopy(statusLabel, sizeof(statusLabel), "GPU STRESS");
            if (loopLabel[0]) { StrCat2(statusLabel, sizeof(statusLabel), statusLabel, "  "); StrCat2(statusLabel, sizeof(statusLabel), statusLabel, loopLabel); }

            char subMsg[32];
            StrCopy(subMsg, sizeof(subMsg), gpuLabel);

            DrawAutoStatus(logo, statusLabel, step, total, subMsg);

            if (reportOK)
            {
                char secName[48];
                StrCopy(secName, sizeof(secName), "GPU STRESS TEST");
                if (loopLabel[0]) { StrCat2(secName, sizeof(secName), secName, " - "); StrCat2(secName, sizeof(secName), secName, loopLabel); }
                RSection(hf, secName);
                GpuStress_AutoRun(hf, gpuMs);
                DWORD _w; WriteFile(hf, "\r\n", 2, &_w, NULL);
            }
            else
            {
                GpuStress_AutoRun(INVALID_HANDLE_VALUE, gpuMs);
            }

            // Accumulate per-loop GPU results
            acc_gpuLoops += GpuAutoRun_GetLoops();
            float lPeak = GpuAutoRun_GetPeakFPS();
            float lMin = GpuAutoRun_GetMinFPS();
            if (lPeak > acc_gpuPeak) acc_gpuPeak = lPeak;
            if (lMin < acc_gpuMin)  acc_gpuMin = lMin;
            ++acc_gpuRuns;

            ++step;
        }
    }

    // ---- Multi-loop summary (only written when loops > 1) ----

    if (loops > 1 && reportOK && (doCpu || doRam || doGpu))
    {
        DWORD w;
        RSection(hf, "STRESS SUMMARY");
        char line[128];

        auto WS = [&](const char* lbl, const char* val)
            {
                StrCopy(line, sizeof(line), lbl);
                StrCat2(line, sizeof(line), line, val);
                StrCat2(line, sizeof(line), line, "\r\n");
                WriteFile(hf, line, StrLen(line), &w, NULL);
            };

        char t[32];
        IntToStr(loops, t, sizeof(t)); WS("Total loops:         ", t);

        if (doCpu && acc_cpuLoops > 0)
        {
            // CPU temp range across all loops
            char mn[8], mx[8];
            IntToStr((int)acc_cpuMin, mn, sizeof(mn));
            IntToStr((int)acc_cpuMax, mx, sizeof(mx));
            StrCopy(t, sizeof(t), mn); StrCat2(t, sizeof(t), t, " C  ->  ");
            StrCat2(t, sizeof(t), t, mx); StrCat2(t, sizeof(t), t, " C");
            WS("CPU temp range:      ", t);

            // Average peak CPU temp across loops
            char av[8];
            IntToStr((int)(acc_cpuPeakSum / (DWORD)acc_cpuLoops), av, sizeof(av));
            StrCopy(t, sizeof(t), av); StrCat2(t, sizeof(t), t, " C");
            WS("CPU avg peak temp:   ", t);

            // Fan range across all loops
            if (acc_fanMin != 255)
            {
                char fnMin[8], fnMax[8];
                IntToStr((int)acc_fanMin * 2, fnMin, sizeof(fnMin));
                IntToStr((int)acc_fanMax * 2, fnMax, sizeof(fnMax));
                StrCopy(t, sizeof(t), fnMin); StrCat2(t, sizeof(t), t, "%  ->  ");
                StrCat2(t, sizeof(t), t, fnMax); StrCat2(t, sizeof(t), t, "%");
                WS("Fan speed range:     ", t);
            }
            WS("Thermal abort (any): ", acc_thermalAny ? "YES" : "No");
            WS("CPU stress result:   ",
                acc_thermalAny ? "ABORTED" :
                (acc_cpuMax >= 85 ? "WARNING - peak above 85C" : "PASS"));
        }

        if (doRam && acc_ramLoops > 0)
        {
            char sw[12], er[12];
            IntToStr((int)acc_ramSweeps, sw, sizeof(sw));
            IntToStr((int)acc_ramErrors, er, sizeof(er));
            WS("RAM total sweeps:    ", sw);
            WS("RAM total errors:    ", er);
            WS("RAM stress result:   ", acc_ramFailed ? "FAIL - errors detected" : "PASS");
        }

        if (doGpu && acc_gpuRuns > 0)
        {
            char gl[8], pk[12], mn[12];
            IntToStr((int)acc_gpuLoops, gl, sizeof(gl));
            WS("GPU scene loops:     ", gl);

            char pkBuf[8]; IntToStr((int)acc_gpuPeak, pkBuf, sizeof(pkBuf));
            StrCopy(pk, sizeof(pk), pkBuf); StrCat2(pk, sizeof(pk), pk, " fps");
            WS("GPU peak FPS:        ", pk);

            int iMin = (acc_gpuMin < 9000.f) ? (int)acc_gpuMin : 0;
            char mnBuf[8]; IntToStr(iMin, mnBuf, sizeof(mnBuf));
            StrCopy(mn, sizeof(mn), mnBuf); StrCat2(mn, sizeof(mn), mn, " fps");
            WS("GPU min FPS:         ", mn);

            WS("GPU stress result:   ",
                (acc_gpuMin >= 20.f) ? "PASS" :
                (acc_gpuMin >= 10.f) ? "WARNING - min FPS below 20" :
                "FAIL - min FPS critically low");
        }

        WriteFile(hf, "\r\n", 2, &w, NULL);
    }

    // ---- Close report ----

    if (reportOK)
    {
        DWORD w;
        const char* footer = "\r\n====================================\r\nEnd of report\r\n";
        WriteFile(hf, footer, StrLen(footer), &w, NULL);
        FlushFileBuffers(hf);
        CloseHandle(hf);
    }

    // ---- Completion screen with optional shutdown countdown ----

    const bool willShutdown = g_autoSettings.shutdownAfter;
    DrawAutoComplete(logo, reportOK, willShutdown);

    if (willShutdown)
    {
        // 30-second countdown before shutdown — [B] aborts
        DWORD shutStart = GetTickCount();
        WORD  prev = GetButtons();
        bool  aborted = false;

        while (!aborted)
        {
            PumpInput();
            DWORD elapsed = GetTickCount() - shutStart;
            if (elapsed >= 30000) break;  // countdown expired — shut down

            DWORD remain = (30000 - elapsed) / 1000 + 1;

            g_pDevice->BeginScene();
            DrawPageChrome(logo, "AUTO DIAGNOSTIC", "[B] Cancel shutdown");
            float py = CONTENT_Y + 40.f;
            if (reportOK)
            {
                DrawText(LM, py, "DIAGNOSTICS COMPLETE", 1.6f, COL_GREEN);
                py += LINE_H + 8.f;
                DrawText(LM, py, "Results saved to D:\\XbDiag.txt", 1.2f, COL_CYAN);
            }
            else
            {
                DrawText(LM, py, "DIAGNOSTICS COMPLETE", 1.6f, COL_ORANGE);
                py += LINE_H + 8.f;
                DrawText(LM, py, "Could not write D:\\XbDiag.txt", 1.2f, COL_RED);
            }
            py += LINE_H * 2.f;
            DrawText(LM, py, "Shutting down in:", 1.3f, COL_YELLOW);
            py += LINE_H + 4.f;
            char secBuf[8]; IntToStr((int)remain, secBuf, sizeof(secBuf));
            DrawText(LM, py, secBuf, 3.5f, COL_CYAN);
            py += LINE_H * 3.5f;
            DrawText(LM, py, "Press [B] to cancel and return to the menu.", 1.2f, COL_DIM);
            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);

            WORD cur = GetButtons();
            if ((cur & BTN_B) && !(prev & BTN_B)) aborted = true;
            prev = cur;
        }

        if (!aborted)
            XbShutdown();
        else
            RequestState(MSTATE_MENU);
    }
    else
    {
        // No shutdown — simple A/B wait
        DrawAutoComplete(logo, reportOK, false);
        WORD prev = GetButtons();
        while (true)
        {
            PumpInput();
            WORD cur = GetButtons();
            if (((cur & BTN_A) && !(prev & BTN_A)) ||
                ((cur & BTN_B) && !(prev & BTN_B))) break;
            prev = cur;
            DrawAutoComplete(logo, reportOK, false);
        }
        RequestState(MSTATE_MENU);
    }
}

// ============================================================================
// Settings editor UI
// ============================================================================

// Row types
enum RowType {
    RT_BOOL, RT_CPU_HRS, RT_CPU_MIN, RT_RAM_HRS, RT_RAM_MIN,
    RT_GPU_HRS, RT_GPU_MIN, RT_LOOPS, RT_ALT, RT_SHUTDOWN
};

struct SettingRow
{
    const char* label;
    const char* desc;
    bool* pVal;   // non-null for RT_BOOL / RT_ALT / RT_SHUTDOWN
    RowType     type;
};

static SettingRow s_rows[] =
{
    { "SYSTEM INFO",    "Full hardware snapshot",                   &g_autoSettings.runSysInfo,   RT_BOOL    },
    { "VIDEO INFO",     "Encoder, AV pack, resolution",             &g_autoSettings.runVideoInfo, RT_BOOL    },
    { "SMBUS SCAN",     "Scan all SMBus addresses",                 &g_autoSettings.runSmBus,     RT_BOOL    },
    { "HDD INFO",       "ATA identify + sequential benchmark",      &g_autoSettings.runHddInfo,   RT_BOOL    },
    { "RAM TEST",       "One full quick sweep across all banks",    &g_autoSettings.runRamTest,   RT_BOOL    },
    { "TEMP MONITOR",   "5 temperature samples at 500ms intervals", &g_autoSettings.runTempMon,   RT_BOOL    },
    { "EEPROM",         "Read and decode EEPROM contents",          &g_autoSettings.runEeprom,    RT_BOOL    },
    { "CTRL TEST",      "Snapshot: port/MU presence, stick pos, stuck buttons",  &g_autoSettings.runCtrlTest,  RT_BOOL    },
    { "CPU STRESS",     "Timed CPU FPU burn",                       &g_autoSettings.runCpuStress, RT_BOOL    },
    { "  CPU HOURS",    "CPU stress hours  [Left/Right]",           NULL,                         RT_CPU_HRS },
    { "  CPU MINS",     "CPU stress minutes  [Left/Right]",         NULL,                         RT_CPU_MIN },
    { "RAM STRESS",     "Repeated RAM sweeps",                      &g_autoSettings.runRamStress, RT_BOOL    },
    { "  RAM HOURS",    "RAM stress hours  [Left/Right]",           NULL,                         RT_RAM_HRS },
    { "  RAM MINS",     "RAM stress minutes  [Left/Right]",         NULL,                         RT_RAM_MIN },
    { "GPU STRESS",     "Crystalline Grotto scene loop (NV2A load)",&g_autoSettings.runGpuStress, RT_BOOL    },
    { "  GPU HOURS",    "GPU stress hours  [Left/Right]",           NULL,                         RT_GPU_HRS },
    { "  GPU MINS",     "GPU stress minutes  [Left/Right]",         NULL,                         RT_GPU_MIN },
    { "STRESS LOOPS",   "Repeat stress sequence N times  [Left/Right]", NULL,                    RT_LOOPS   },
    { "ALT STRESS",     "Interleave CPU+RAM+GPU loops",             &g_autoSettings.altStressMode, RT_ALT    },
    { "SHUTDOWN AFTER", "Power off Xbox on autorun completion",     &g_autoSettings.shutdownAfter, RT_SHUTDOWN },
};
static const int k_rowCount = sizeof(s_rows) / sizeof(s_rows[0]);

static int  s_sel = 0;
static int  s_scrollTop = 0;   // index of first visible row
static bool s_saved = false;
static bool s_saveFail = false;
static bool s_deleted = false;
static WORD s_prevBtns = 0;

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

void XbSet_OnEnter()
{
    s_sel = 0;
    s_scrollTop = 0;
    s_saved = false;
    s_saveFail = false;
    s_deleted = false;
    s_prevBtns = GetButtons();
}

static void RenderXbSet(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    const char* hint;
    if (s_deleted)
        hint = "[A] Toggle    [Y] Save    [X] Settings deleted    [B] Back";
    else if (s_saved)
        hint = "[A] Toggle    [Y] Saved OK    [X] Delete file    [B] Back";
    else if (s_saveFail)
        hint = "[A] Toggle    [Y] Save FAILED    [X] Delete file    [B] Back";
    else
        hint = "[A] Toggle    [Y] Save    [X] Delete XbDiag.set    [B] Back";

    DrawPageChrome(logo, "AUTOMATION SETTINGS", hint);

    const float ROW_H = 22.f;
    const float LBL_X = LM;
    const float CHK_X = SW - LM - 80.f;
    const float DESC_X = LM + 200.f;
    float y = CONTENT_Y + 4.f;

    DrawText(LBL_X, y, "MODULE / SETTING", 1.1f, COL_DIM);
    DrawText(CHK_X, y, "VALUE", 1.1f, COL_DIM);
    HLine(y + LINE_H, LBL_X, SW - LM, COL_BORDER);
    y += LINE_H + 4.f;

    // Compute how many rows fit in the usable area
    const float usableH = BOT_BAR_Y - y;
    const int visRows = (int)(usableH / ROW_H);

    // Keep selection visible — scroll window tracks s_sel
    if (s_sel < s_scrollTop) s_scrollTop = s_sel;
    if (s_sel >= s_scrollTop + visRows) s_scrollTop = s_sel - visRows + 1;
    if (s_scrollTop < 0) s_scrollTop = 0;

    // Scroll indicator arrows
    if (s_scrollTop > 0)
        DrawText(SW - LM - 16.f, CONTENT_Y + 6.f, "\x1E", 1.2f, COL_DIM); // up arrow hint
    if (s_scrollTop + visRows < k_rowCount)
        DrawText(SW - LM - 16.f, BOT_BAR_Y - LINE_H - 2.f, "\x1F", 1.2f, COL_DIM); // down arrow hint

    const int iEnd = (s_scrollTop + visRows < k_rowCount) ? s_scrollTop + visRows : k_rowCount;
    for (int i = s_scrollTop; i < iEnd; ++i)
    {
        bool sel = (i == s_sel);
        if (sel)
        {
            FillRectGrad(0.f, y - 2.f, SW, y + ROW_H - 3.f,
                COL_SEL_BAR, COL_SEL_BAR2);
            FillRect(0.f, y - 2.f, 4.f, y + ROW_H - 3.f,
                D3DCOLOR_XRGB(80, 140, 255));
        }

        RowType rt = s_rows[i].type;
        DWORD lblCol = sel ? COL_WHITE : COL_GRAY;

        // Indented sub-rows (CPU/RAM duration) get a slightly dimmer base colour
        if (rt == RT_CPU_HRS || rt == RT_CPU_MIN || rt == RT_RAM_HRS || rt == RT_RAM_MIN ||
            rt == RT_GPU_HRS || rt == RT_GPU_MIN)
            lblCol = sel ? COL_YELLOW : D3DCOLOR_XRGB(140, 130, 80);

        DrawText(LBL_X, y, s_rows[i].label, 1.2f, lblCol);
        if (sel) DrawText(DESC_X, y, s_rows[i].desc, 1.0f, COL_DIM);

        if (rt == RT_BOOL || rt == RT_ALT || rt == RT_SHUTDOWN)
        {
            bool val = *s_rows[i].pVal;
            DrawText(CHK_X, y, val ? "[ ON ]" : "[ OFF]", 1.25f,
                val ? COL_GREEN : COL_RED);
        }
        else if (rt == RT_CPU_HRS)
        {
            char vb[8]; IntToStr(g_autoSettings.cpuStressHours, vb, sizeof(vb));
            DrawText(CHK_X, y, vb, 1.35f, COL_CYAN);
        }
        else if (rt == RT_CPU_MIN)
        {
            char vb[8]; IntToStr(g_autoSettings.cpuStressMins, vb, sizeof(vb));
            DrawText(CHK_X, y, vb, 1.35f, COL_CYAN);
        }
        else if (rt == RT_RAM_HRS)
        {
            char vb[8]; IntToStr(g_autoSettings.ramStressHours, vb, sizeof(vb));
            DrawText(CHK_X, y, vb, 1.35f, COL_CYAN);
        }
        else if (rt == RT_RAM_MIN)
        {
            char vb[8]; IntToStr(g_autoSettings.ramStressMins, vb, sizeof(vb));
            DrawText(CHK_X, y, vb, 1.35f, COL_CYAN);
        }
        else if (rt == RT_GPU_HRS)
        {
            char vb[8]; IntToStr(g_autoSettings.gpuStressHours, vb, sizeof(vb));
            DrawText(CHK_X, y, vb, 1.35f, COL_CYAN);
        }
        else if (rt == RT_GPU_MIN)
        {
            char vb[8]; IntToStr(g_autoSettings.gpuStressMins, vb, sizeof(vb));
            DrawText(CHK_X, y, vb, 1.35f, COL_CYAN);
        }
        else if (rt == RT_LOOPS)
        {
            char vb[8]; IntToStr(g_autoSettings.stressLoops, vb, sizeof(vb));
            DrawText(CHK_X, y, vb, 1.35f, COL_CYAN);
        }

        HLine(y + ROW_H - 3.f, 0.f, SW, D3DCOLOR_XRGB(18, 22, 45));
        y += ROW_H;
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

void XbSet_Tick(const DiagLogo& logo)
{
    WORD cur = GetButtons();

    if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
    {
        s_prevBtns = cur;
        RequestState(MSTATE_MENU);
        return;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_UP))
    {
        if (--s_sel < 0) s_sel = k_rowCount - 1;
        s_saved = false;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_DOWN))
    {
        if (++s_sel >= k_rowCount) s_sel = 0;
        s_saved = false;
    }

    RowType rt = s_rows[s_sel].type;

    if (EdgeDown(cur, s_prevBtns, BTN_A))
    {
        if (s_rows[s_sel].pVal && (rt == RT_BOOL || rt == RT_ALT || rt == RT_SHUTDOWN))
            *s_rows[s_sel].pVal = !*s_rows[s_sel].pVal;
        s_saved = false;
    }

    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_RIGHT))
    {
        s_saved = false;
        if (rt == RT_CPU_HRS) { if (g_autoSettings.cpuStressHours < 99) ++g_autoSettings.cpuStressHours; }
        if (rt == RT_CPU_MIN)
        {
            if (g_autoSettings.cpuStressMins < 59) ++g_autoSettings.cpuStressMins;
            else { g_autoSettings.cpuStressMins = 0; if (g_autoSettings.cpuStressHours < 99) ++g_autoSettings.cpuStressHours; }
        }
        if (rt == RT_RAM_HRS) { if (g_autoSettings.ramStressHours < 99) ++g_autoSettings.ramStressHours; }
        if (rt == RT_RAM_MIN)
        {
            if (g_autoSettings.ramStressMins < 59) ++g_autoSettings.ramStressMins;
            else { g_autoSettings.ramStressMins = 0; if (g_autoSettings.ramStressHours < 99) ++g_autoSettings.ramStressHours; }
        }
        if (rt == RT_GPU_HRS) { if (g_autoSettings.gpuStressHours < 99) ++g_autoSettings.gpuStressHours; }
        if (rt == RT_GPU_MIN)
        {
            if (g_autoSettings.gpuStressMins < 59) ++g_autoSettings.gpuStressMins;
            else { g_autoSettings.gpuStressMins = 0; if (g_autoSettings.gpuStressHours < 99) ++g_autoSettings.gpuStressHours; }
        }
        if (rt == RT_LOOPS) { if (g_autoSettings.stressLoops < 99) ++g_autoSettings.stressLoops; }
    }

    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_LEFT))
    {
        s_saved = false;
        if (rt == RT_CPU_HRS) { if (g_autoSettings.cpuStressHours > 0) --g_autoSettings.cpuStressHours; }
        if (rt == RT_CPU_MIN)
        {
            if (g_autoSettings.cpuStressMins > 0) --g_autoSettings.cpuStressMins;
            else if (g_autoSettings.cpuStressHours > 0) { --g_autoSettings.cpuStressHours; g_autoSettings.cpuStressMins = 59; }
        }
        if (rt == RT_RAM_HRS) { if (g_autoSettings.ramStressHours > 0) --g_autoSettings.ramStressHours; }
        if (rt == RT_RAM_MIN)
        {
            if (g_autoSettings.ramStressMins > 0) --g_autoSettings.ramStressMins;
            else if (g_autoSettings.ramStressHours > 0) { --g_autoSettings.ramStressHours; g_autoSettings.ramStressMins = 59; }
        }
        if (rt == RT_GPU_HRS) { if (g_autoSettings.gpuStressHours > 0) --g_autoSettings.gpuStressHours; }
        if (rt == RT_GPU_MIN)
        {
            if (g_autoSettings.gpuStressMins > 0) --g_autoSettings.gpuStressMins;
            else if (g_autoSettings.gpuStressHours > 0) { --g_autoSettings.gpuStressHours; g_autoSettings.gpuStressMins = 59; }
        }
        if (rt == RT_LOOPS) { if (g_autoSettings.stressLoops > 1) --g_autoSettings.stressLoops; }
    }

    if (EdgeDown(cur, s_prevBtns, BTN_Y))
    {
        s_saved = XbSet_SaveSettings();
        s_saveFail = !s_saved;
        g_autoSettingsFound = true;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_X))
    {
        if (XbSet_DeleteSettings())
        {
            s_saved = false;
            s_saveFail = false;
            s_deleted = true;
        }
    }

    s_prevBtns = cur;
    RenderXbSet(logo);
}