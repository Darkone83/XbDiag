// XbSet.cpp
// XbDiag automation settings editor and autorun engine.
//
// Settings file format (D:\XbDiag.set):
//   KEY=0 or KEY=1 per line, STRESS_DUR=<index>
//   Unrecognised keys ignored. CRLF or LF.
//
// Autorun sequence:
//   SysInfo -> VideoInfo -> SmBusScan -> HddInfo -> RamTest -> TempMon
//   -> EepromView -> ControllerTest (60s wait) -> CPU Stress -> RAM Stress
//
// Controller test waits up to 60 seconds for [A] to confirm presence.
// If no input arrives, it is skipped and the report notes "No input / skipped".
//
// Stress tests run for stressDuration seconds (30/60/120/300).
// Peak CPU temp, min/max load, and fan speed are logged.

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
#include "StressMath.h"
// StressTest_AutoRun declared in StressTest.h
extern void StressTest_AutoRun(HANDLE hReport, DWORD durationMs);
#include <xtl.h>
#include <winsockx.h>

extern void RequestState(int newState);

static const int MSTATE_MENU = 0;

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

// ============================================================================
// Settings I/O
// ============================================================================

bool XbSet_LoadSettings()
{
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
    g_autoSettings.stressHours = 0;
    g_autoSettings.stressMins = 30;  // default 30 minutes

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

        if (StrEq(key, "SYSINFO"))    g_autoSettings.runSysInfo = on;
        if (StrEq(key, "VIDEOINFO"))  g_autoSettings.runVideoInfo = on;
        if (StrEq(key, "SMBUS"))      g_autoSettings.runSmBus = on;
        if (StrEq(key, "HDDINFO"))    g_autoSettings.runHddInfo = on;
        if (StrEq(key, "RAMTEST"))    g_autoSettings.runRamTest = on;
        if (StrEq(key, "TEMPMON"))    g_autoSettings.runTempMon = on;
        if (StrEq(key, "EEPROM"))     g_autoSettings.runEeprom = on;
        if (StrEq(key, "CTRLTEST"))   g_autoSettings.runCtrlTest = on;
        if (StrEq(key, "CPUSTRESS"))  g_autoSettings.runCpuStress = on;
        if (StrEq(key, "RAMSTRESS"))  g_autoSettings.runRamStress = on;
        if (StrEq(key, "STRESS_HRS"))
        {
            int v = 0;
            for (int i = 0; val[i] >= '0' && val[i] <= '9'; ++i) v = v * 10 + (val[i] - '0');
            if (v >= 0 && v <= 99) g_autoSettings.stressHours = v;
        }
        if (StrEq(key, "STRESS_MIN"))
        {
            int v = 0;
            for (int i = 0; val[i] >= '0' && val[i] <= '9'; ++i) v = v * 10 + (val[i] - '0');
            if (v >= 0 && v <= 59) g_autoSettings.stressMins = v;
        }
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

    // Stress duration index
    StrCopy(line, sizeof(line), "STRESS_HRS=");
    {
        char v[8]; IntToStr(g_autoSettings.stressHours, v, sizeof(v));
        StrCat2(line, sizeof(line), line, v); StrCat2(line, sizeof(line), line, "\r\n");
    }
    WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "STRESS_MIN=");
    {
        char v[8]; IntToStr(g_autoSettings.stressMins, v, sizeof(v));
        StrCat2(line, sizeof(line), line, v); StrCat2(line, sizeof(line), line, "\r\n");
    }
    WriteFile(hf, line, StrLen(line), &w, NULL);

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

static void DrawAutoComplete(const DiagLogo& logo, bool ok)
{
    g_pDevice->BeginScene();
    DrawPageChrome(logo, "AUTO DIAGNOSTIC", "[A] Return to menu");
    float py = CONTENT_Y + 50.f;
    if (ok)
    {
        DrawText(LM, py, "DIAGNOSTICS COMPLETE", 1.6f, COL_GREEN); py += LINE_H + 8.f;
        DrawText(LM, py, "Results saved to XbDiag.txt", 1.2f, COL_CYAN);
    }
    else
    {
        DrawText(LM, py, "DIAGNOSTICS COMPLETE", 1.6f, COL_ORANGE); py += LINE_H + 8.f;
        DrawText(LM, py, "Could not write XbDiag.txt", 1.2f, COL_RED);
    }
    py += LINE_H * 2.f;
    DrawText(LM, py, "Press [A] or [B] to return to the main menu.", 1.2f, COL_GRAY);
    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// Controller test with 60-second user-input wait
// Displays a prompt, counts down. [A] = proceed, countdown expiry = skip.
// Returns true if the test ran, false if skipped.
// ============================================================================

static bool CtrlTest_WithTimeout(const DiagLogo& logo,
    int step, int total, HANDLE hReport)
{
    DWORD waitStart = GetTickCount();
    WORD  prev = GetButtons();

    while (true)
    {
        PumpInput();
        DWORD elapsed = GetTickCount() - waitStart;
        DWORD remain = (elapsed < 60000) ? (60000 - elapsed) / 1000 + 1 : 0;

        // Render wait prompt
        g_pDevice->BeginScene();
        DrawPageChrome(logo, "AUTO DIAGNOSTIC", "[A] Confirm    [B] Skip");

        float py = CONTENT_Y + 30.f;
        char stepBuf[32]; char sa[8], sb[8];
        IntToStr(step, sa, sizeof(sa)); IntToStr(total, sb, sizeof(sb));
        StrCopy(stepBuf, sizeof(stepBuf), "Step ");
        StrCat2(stepBuf, sizeof(stepBuf), stepBuf, sa);
        StrCat2(stepBuf, sizeof(stepBuf), stepBuf, " of ");
        StrCat2(stepBuf, sizeof(stepBuf), stepBuf, sb);
        DrawText(LM, py, stepBuf, 1.1f, COL_GRAY); py += LINE_H + 4.f;

        DrawText(LM, py, "CONTROLLER TEST", 1.7f, COL_CYAN); py += LINE_H + 8.f;
        DrawText(LM, py, "Connect controllers and press [A] to confirm.", 1.2f, COL_WHITE);
        py += LINE_H + 4.f;
        DrawText(LM, py, "Press [B] to skip this test.", 1.2f, COL_GRAY);
        py += LINE_H + 10.f;

        char secBuf[32];
        StrCopy(secBuf, sizeof(secBuf), "Skipping in ");
        char t[8]; IntToStr((int)remain, t, sizeof(t));
        StrCat2(secBuf, sizeof(secBuf), secBuf, t);
        StrCat2(secBuf, sizeof(secBuf), secBuf, "s...");
        DrawText(LM, py, secBuf, 1.2f, COL_ORANGE);

        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);

        WORD cur = GetButtons();
        bool edgeA = ((cur & BTN_A) && !(prev & BTN_A));
        bool edgeB = ((cur & BTN_B) && !(prev & BTN_B));
        prev = cur;

        if (edgeA)
        {
            // User confirmed — run test
            ControllerTest_OnEnter();
            RSection(hReport, "CONTROLLER TEST");
            ControllerTest_AutoRun(hReport);
            return true;
        }
        if (edgeB || elapsed >= 60000)
        {
            RSection(hReport, "CONTROLLER TEST");
            RW(hReport, "Result:       ", "No input / skipped");
            return false;
        }
    }
}

// ============================================================================
// Stress test AutoRun — timed CPU or RAM stress
// ============================================================================

// Access StressTest internals via extern
extern BYTE  StressAutoRun_GetCurCPU();
extern BYTE  StressAutoRun_GetMinCPU();
extern BYTE  StressAutoRun_GetMaxCPU();
extern BYTE  StressAutoRun_GetMeasuredLoad();
extern BYTE  StressAutoRun_GetMinFan();
extern BYTE  StressAutoRun_GetMaxFan();
extern bool  StressAutoRun_GetThermalAbort();

static void RunCpuStressAuto(const DiagLogo& logo,
    int step, int total, DWORD durationMs, HANDLE hReport)
{
    // Delegates to StressTest_AutoRun — uses the real StressTest.cpp
    // burn loop (same CPUStress kernels, same sensor path, same
    // edge-aligned measurement windows as the interactive stress test).
    StressTest_AutoRun(hReport, durationMs);
}

static void RunRamStressAuto(const DiagLogo& logo,
    int step, int total, DWORD durationMs, HANDLE hReport)
{
    // Delegates to RamStress_AutoRun in StressTest.cpp —
    // uses the real StressTest RAM stress engine (RamOnStart /
    // RamStressStep / RamStop) running for the configured duration.
    extern void RamStress_AutoRun(HANDLE hReport, DWORD durationMs);
    RamStress_AutoRun(hReport, durationMs);
}

// ============================================================================
// AutoRun engine
// ============================================================================

void XbSet_AutoRun(const DiagLogo& logo)
{
    // Count enabled modules
    int total = 0;
    if (g_autoSettings.runSysInfo)   ++total;
    if (g_autoSettings.runVideoInfo) ++total;
    if (g_autoSettings.runSmBus)     ++total;
    if (g_autoSettings.runHddInfo)   ++total;
    if (g_autoSettings.runRamTest)   ++total;
    if (g_autoSettings.runTempMon)   ++total;
    if (g_autoSettings.runEeprom)    ++total;
    if (g_autoSettings.runCtrlTest)  ++total;
    if (g_autoSettings.runCpuStress) ++total;
    if (g_autoSettings.runRamStress) ++total;

    if (total == 0) { RequestState(MSTATE_MENU); return; }


    DWORD stressTotalSecs = (DWORD)(g_autoSettings.stressHours * 3600
        + g_autoSettings.stressMins * 60);
    if (stressTotalSecs < 60) stressTotalSecs = 60;
    DWORD stressMs = stressTotalSecs * 1000;

    static char s_durLabel[16];
    if (g_autoSettings.stressHours > 0)
    {
        char hh[8], mm[8];
        IntToStr(g_autoSettings.stressHours, hh, sizeof(hh));
        IntToStr(g_autoSettings.stressMins, mm, sizeof(mm));
        StrCopy(s_durLabel, sizeof(s_durLabel), hh);
        StrCat2(s_durLabel, sizeof(s_durLabel), s_durLabel, "h ");
        StrCat2(s_durLabel, sizeof(s_durLabel), s_durLabel, mm);
        StrCat2(s_durLabel, sizeof(s_durLabel), s_durLabel, "m");
    }
    else
    {
        char mm[8]; IntToStr(g_autoSettings.stressMins, mm, sizeof(mm));
        StrCopy(s_durLabel, sizeof(s_durLabel), mm);
        StrCat2(s_durLabel, sizeof(s_durLabel), s_durLabel, "m");
    }

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

        // Log stress duration setting
        char durLine[48];
        StrCopy(durLine, sizeof(durLine), "Stress duration: ");
        StrCat2(durLine, sizeof(durLine), durLine,
            s_durLabel);
        StrCat2(durLine, sizeof(durLine), durLine, "\r\n\r\n");
        WriteFile(hf, durLine, StrLen(durLine), &w, NULL);
    }

    int step = 1;

    if (g_autoSettings.runSysInfo)
    {
        DrawAutoStatus(logo, "SYSTEM INFO", step++, total);
        SysInfo_OnEnter();
        if (reportOK) {
            RSection(hf, "SYSTEM INFO"); SysInfo_AutoRun(hf);
            DWORD _w1; WriteFile(hf, "\r\n", 2, &_w1, NULL);
        }
    }
    if (g_autoSettings.runVideoInfo)
    {
        DrawAutoStatus(logo, "VIDEO INFO", step++, total);
        VideoInfo_OnEnter();
        if (reportOK) {
            RSection(hf, "VIDEO INFO"); VideoInfo_AutoRun(hf);
            DWORD _w2; WriteFile(hf, "\r\n", 2, &_w2, NULL);
        }
    }
    if (g_autoSettings.runSmBus)
    {
        DrawAutoStatus(logo, "SMBUS SCAN", step++, total);
        SmBusScan_OnEnter();
        if (reportOK) {
            RSection(hf, "SMBUS SCAN"); SmBusScan_AutoRun(hf);
            DWORD _w3; WriteFile(hf, "\r\n", 2, &_w3, NULL);
        }
    }
    if (g_autoSettings.runHddInfo)
    {
        DrawAutoStatus(logo, "HDD INFO + BENCHMARK", step++, total);
        HddInfo_OnEnter();
        if (reportOK) {
            RSection(hf, "HDD INFO"); HddInfo_AutoRun(hf);
            DWORD _w4; WriteFile(hf, "\r\n", 2, &_w4, NULL);
        }
    }
    if (g_autoSettings.runRamTest)
    {
        DrawAutoStatus(logo, "RAM TEST", step++, total);
        RamTest_OnEnter();
        if (reportOK) {
            RSection(hf, "RAM TEST"); RamTest_AutoRun(hf);
            DWORD _w5; WriteFile(hf, "\r\n", 2, &_w5, NULL);
        }
    }
    if (g_autoSettings.runTempMon)
    {
        DrawAutoStatus(logo, "TEMP MONITOR", step++, total);
        TempMonitor_OnEnter();
        if (reportOK) {
            RSection(hf, "TEMP MONITOR"); TempMonitor_AutoRun(hf);
            DWORD _w6; WriteFile(hf, "\r\n", 2, &_w6, NULL);
        }
    }
    if (g_autoSettings.runEeprom)
    {
        DrawAutoStatus(logo, "EEPROM", step++, total);
        EepromView_OnEnter();
        if (reportOK) {
            RSection(hf, "EEPROM"); EepromView_AutoRun(hf);
            DWORD _w7; WriteFile(hf, "\r\n", 2, &_w7, NULL);
        }
    }
    if (g_autoSettings.runCtrlTest)
    {
        // Interactive — shows 60s countdown, skips with note if no input
        if (reportOK)
            CtrlTest_WithTimeout(logo, step, total, hf);
        else
            CtrlTest_WithTimeout(logo, step, total, INVALID_HANDLE_VALUE);
        ++step;
    }
    if (g_autoSettings.runCpuStress)
    {
        DrawAutoStatus(logo, "CPU STRESS TEST", step, total, s_durLabel);
        if (reportOK)
        {
            RSection(hf, "CPU STRESS TEST");
            RunCpuStressAuto(logo, step, total, stressMs, hf);
            DWORD w; const char* nl = "\r\n";
            WriteFile(hf, nl, 2, &w, NULL);
        }
        ++step;
    }
    if (g_autoSettings.runRamStress)
    {
        DrawAutoStatus(logo, "RAM STRESS TEST", step, total, s_durLabel);
        if (reportOK)
        {
            RSection(hf, "RAM STRESS TEST");
            RunRamStressAuto(logo, step, total, stressMs, hf);
            DWORD w; const char* nl = "\r\n";
            WriteFile(hf, nl, 2, &w, NULL);
        }
        ++step;
    }

    if (reportOK)
    {
        DWORD w;
        const char* footer = "\r\n====================================\r\nEnd of report\r\n";
        WriteFile(hf, footer, StrLen(footer), &w, NULL);
        FlushFileBuffers(hf);
        CloseHandle(hf);
    }

    DrawAutoComplete(logo, reportOK);
    {
        WORD prev = GetButtons();
        while (true)
        {
            PumpInput();
            WORD cur = GetButtons();
            if (((cur & BTN_A) && !(prev & BTN_A)) ||
                ((cur & BTN_B) && !(prev & BTN_B))) break;
            prev = cur;
            DrawAutoComplete(logo, reportOK);
        }
    }
    RequestState(MSTATE_MENU);
}

// ============================================================================
// Settings editor UI
// ============================================================================

struct SettingRow
{
    const char* label;
    const char* desc;
    bool* pVal;
    bool        isDur;   // unused, kept for compat
    bool        isHrs;
    bool        isMins;
};

static SettingRow s_rows[] =
{
    { "SYSTEM INFO",    "Full hardware snapshot",                    &g_autoSettings.runSysInfo,   false , false, false },
    { "VIDEO INFO",     "Encoder, AV pack, resolution",              &g_autoSettings.runVideoInfo, false , false, false },
    { "SMBUS SCAN",     "Scan all SMBus addresses",                  &g_autoSettings.runSmBus,     false , false, false },
    { "HDD INFO",       "ATA identify + sequential benchmark",        &g_autoSettings.runHddInfo,   false , false, false },
    { "RAM TEST",       "One full quick sweep across all banks",     &g_autoSettings.runRamTest,   false , false, false },
    { "TEMP MONITOR",   "5 temperature samples at 500ms intervals",  &g_autoSettings.runTempMon,   false , false, false },
    { "EEPROM",         "Read and decode EEPROM contents",           &g_autoSettings.runEeprom,    false , false, false },
    { "CTRL TEST",      "60s wait for input, skip if none",          &g_autoSettings.runCtrlTest,  false , false, false },
    { "CPU STRESS",     "Timed CPU FPU burn (see duration below)",   &g_autoSettings.runCpuStress, false , false, false },
    { "RAM STRESS",     "Repeated RAM sweeps for duration",          &g_autoSettings.runRamStress, false , false, false },
    { "STRESS HOURS",   "Hours  [Left/Right to adjust]",             NULL, false, true,  false },
    { "STRESS MINS",    "Minutes 0-59  [Left/Right to adjust]",      NULL, false, false, true  },
};
static const int k_rowCount = sizeof(s_rows) / sizeof(s_rows[0]);

static int  s_sel = 0;
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
        hint = "[A] Toggle/Cycle    [Y] Save    [X] Settings deleted    [B] Back";
    else if (s_saved)
        hint = "[A] Toggle/Cycle    [Y] Saved OK    [X] Delete file    [B] Back";
    else if (s_saveFail)
        hint = "[A] Toggle/Cycle    [Y] Save FAILED    [X] Delete file    [B] Back";
    else
        hint = "[A] Toggle/Cycle    [Y] Save    [X] Delete XbDiag.set    [B] Back";

    DrawPageChrome(logo, "AUTOMATION SETTINGS", hint);

    const float ROW_H = 26.f;
    const float LBL_X = LM;
    const float CHK_X = SW - LM - 72.f;
    const float DESC_X = LM + 190.f;
    float y = CONTENT_Y + 4.f;

    DrawText(LBL_X, y, "MODULE", 1.1f, COL_DIM);
    DrawText(CHK_X, y, "SETTING", 1.1f, COL_DIM);
    HLine(y + LINE_H, LBL_X, SW - LM, COL_BORDER);
    y += LINE_H + 4.f;

    for (int i = 0; i < k_rowCount; ++i)
    {
        bool sel = (i == s_sel);
        if (sel)
        {
            FillRectGrad(0.f, y - 2.f, SW, y + ROW_H - 4.f,
                COL_SEL_BAR, COL_SEL_BAR2);
            FillRect(0.f, y - 2.f, 4.f, y + ROW_H - 4.f,
                D3DCOLOR_XRGB(80, 140, 255));
        }

        DWORD lblCol = sel ? COL_WHITE : COL_GRAY;

        if (s_rows[i].isHrs || s_rows[i].isMins)
        {
            DWORD lblC = sel ? COL_YELLOW : D3DCOLOR_XRGB(160, 140, 60);
            DrawText(LBL_X, y, s_rows[i].label, 1.3f, lblC);
            if (sel) DrawText(DESC_X, y, "[Left] - 1    [Right] + 1", 1.0f, COL_DIM);
            int numVal = s_rows[i].isHrs ? g_autoSettings.stressHours : g_autoSettings.stressMins;
            char vb[8]; IntToStr(numVal, vb, sizeof(vb));
            DrawText(CHK_X, y, vb, 1.4f, COL_CYAN);
        }
        else
        {
            bool val = *s_rows[i].pVal;
            DrawText(LBL_X, y, s_rows[i].label, 1.3f, lblCol);
            if (sel) DrawText(DESC_X, y, s_rows[i].desc, 1.0f, COL_DIM);
            DrawText(CHK_X, y, val ? "[ ON ]" : "[ OFF]", 1.3f,
                val ? COL_GREEN : COL_RED);
        }

        HLine(y + ROW_H - 4.f, 0.f, SW, D3DCOLOR_XRGB(18, 22, 45));
        y += ROW_H;
    }

    float noteY = BOT_BAR_Y - 30.f;
    HLine(noteY, LM, SW - LM, COL_BORDER);
    DrawText(LM, noteY + 4.f,
        "File: XbDiag.set     Report: XbDiag.txt     CtrlTest waits 60s for input",
        1.0f, COL_DIM);

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
    if (EdgeDown(cur, s_prevBtns, BTN_A))
    {
        if (s_rows[s_sel].isHrs || s_rows[s_sel].isMins)
        {
            // Use Left/Right dpad to adjust; A does nothing on these rows
        }
        else
        {
            *s_rows[s_sel].pVal = !*s_rows[s_sel].pVal;
        }
        s_saved = false;
    }
    if (s_rows[s_sel].isHrs)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_RIGHT)) { if (g_autoSettings.stressHours < 99) { ++g_autoSettings.stressHours; s_saved = false; } }
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_LEFT)) { if (g_autoSettings.stressHours > 0) { --g_autoSettings.stressHours; s_saved = false; } }
    }
    if (s_rows[s_sel].isMins)
    {
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_RIGHT))
        {
            if (g_autoSettings.stressMins < 59) { ++g_autoSettings.stressMins; s_saved = false; }
            else { g_autoSettings.stressMins = 0; if (g_autoSettings.stressHours < 99) ++g_autoSettings.stressHours; s_saved = false; }
        }
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_LEFT))
        {
            if (g_autoSettings.stressMins > 0) { --g_autoSettings.stressMins; s_saved = false; }
            else if (g_autoSettings.stressHours > 0) { --g_autoSettings.stressHours; g_autoSettings.stressMins = 59; s_saved = false; }
        }
    }
    if (EdgeDown(cur, s_prevBtns, BTN_Y))
    {
        s_saved = XbSet_SaveSettings();
        s_saveFail = !s_saved;
        g_autoSettingsFound = true;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_X))
    {
        // Delete XbDiag.set — disables autorun on next launch
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