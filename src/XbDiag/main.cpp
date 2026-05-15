// main.cpp
// XbDiag - Xbox Hardware Diagnostic Suite
// Entry point, D3D init, logo lifetime, and top-level state machine.
//
// State machine:
//   STATE_MENU     -> DiagMenu (main navigation hub)
//   STATE_SYSINFO  -> SysInfo  (system snapshot, read-once/display)
//   STATE_RAM      -> RamTest  (blocking inner loop, owns its own pump)
//   STATE_SMBUS    -> SmBusScan(live, tick+render each frame)
//   STATE_TEMP     -> TempMonitor (live, tick+render each frame)
//   STATE_EEPROM   -> EepromView  (read-once/display, sub-view toggle)
//   STATE_VIDEO    -> VideoInfo   (read-once/display)
//   STATE_HDD      -> HddInfo     (read-once/display)
//   STATE_ABOUT    -> AboutScreen    (static display)
//   STATE_CTRL     -> ControllerTest (live, tick+render each frame)
//   STATE_STRESS   -> StressTest     (live, tick+render each frame)
//   STATE_FILES    -> FileExplorer   (live, tick+render each frame)
//   STATE_XBSET    -> XbSet          (automation settings, tick+render each frame)
//   STATE_EXIT     -> XLaunchNewImage to dashboard

#include "DiagCommon.h"
#include "font.h"
#include "input.h"

#include "DiagMenu.h"
#include "SysInfo.h"
#include "RamTest.h"
#include "SmBusScan.h"
#include "TempMonitor.h"
#include "EepromView.h"
#include "VideoInfo.h"
#include "HddInfo.h"
#include "AboutScreen.h"
#include "ControllerTest.h"
#include "StressTest.h"
#include "FileExplorer.h"
#include "FtpServ.h"
#include "XbSet.h"
#include "Update.h"
#include "HttpRptSrv.h"
#include "XVoltUdp.h"
#include "ScreenCalib.h"
#include "HttpCap.h"
#include "lcd.h"

#include <xtl.h>

// ============================================================================
// Globals
// ============================================================================

LPDIRECT3D8       g_pD3D = NULL;
LPDIRECT3DDEVICE8 g_pDevice = NULL;

// Logo loaded once at startup, passed to every screen via DrawPageChrome
static DiagLogo g_logo;

// ============================================================================
// App state enum
// ============================================================================

enum AppState
{
    STATE_MENU = 0,
    STATE_SYSINFO,
    STATE_RAM,
    STATE_SMBUS,
    STATE_TEMP,
    STATE_EEPROM,
    STATE_VIDEO,
    STATE_HDD,
    STATE_ABOUT,
    STATE_CTRL,
    STATE_STRESS,
    STATE_FILES,
    STATE_XBSET,
    STATE_UPDATE,
    STATE_EXIT,
};

static AppState g_state = STATE_MENU;
static AppState g_prevState = STATE_MENU;
static bool     g_lcdDataFed = false;   // true once SysInfo data sent to LCD
static WORD     g_lcdPrevBtns = 0;

// Called by any module to return to the main menu
void RequestState(int newState)
{
    g_state = (AppState)newState;
}

// ============================================================================
// Video mode detection
// Reads XGetVideoFlags() and XGetVideoStandard() to pick the best available
// mode.  Priority: 720p > 1080i > 480p > 576i (PAL-I) > 480i (NTSC/PAL-M/PAL60).
// Sets g_sx, g_sy, g_isHD, g_videoModeStr before D3D init.
// SW/SH remain fixed at 640/480 design units - only g_sx/g_sy know the real res.
// Returns the present flags and backbuffer dims to use in CreateDevice.
//
// PAL notes:
//   XC_VIDEO_STANDARD_PAL_I  (0x00800300) — Europe/Australia 576i 50Hz
//   XC_VIDEO_STANDARD_PAL_M  (0x00400400) — Brazil: PAL colour, NTSC timing
//                                            480i 60Hz, treat same as NTSC
//   XC_VIDEO_FLAGS_PAL_60Hz  (0x00400000) — PAL-I console with 60Hz-capable TV:
//                                            outputs 480i 60Hz, treat same as NTSC
//   PAL 4:3 backbuffer = 640×576, NOT 720×576.
//   720×576 is widescreen/anamorphic PAL — incorrect for a diagnostic tool.
// ============================================================================

struct VideoMode
{
    DWORD width;
    DWORD height;
    DWORD presentFlags;   // D3DPRESENTFLAG_* combination
    DWORD refreshHz;
};

// XC_VIDEO_STANDARD_PAL_M is not defined in the Xbox SDK headers.
// Value from EepromEditor (Eeprom.cs VideoStandard enum) — Brazil PAL-M.
#ifndef XC_VIDEO_STANDARD_PAL_M
#define XC_VIDEO_STANDARD_PAL_M 0x00400400
#endif

static VideoMode DetectVideoMode()
{
    DWORD vidStd = XGetVideoStandard();
    DWORD vidFlags = XGetVideoFlags();

    VideoMode m;
    m.refreshHz = 60;

    // PAL-I: Europe/Australia — 576i 50Hz UNLESS the PAL60 flag overrides it.
    // PAL-M: Brazil — NTSC timing (480i 60Hz); treat as NTSC for mode selection.
    bool isPAL_I = (vidStd == XC_VIDEO_STANDARD_PAL_I);
    bool isPAL_M = (vidStd == XC_VIDEO_STANDARD_PAL_M);
    bool hasPAL60 = (vidFlags & XC_VIDEO_FLAGS_PAL_60Hz) != 0;

    // True 576i 50Hz PAL: PAL-I console, no PAL60 override, no HD mode selected
    bool isTruePAL50 = isPAL_I && !hasPAL60;

    bool has720p = (vidFlags & XC_VIDEO_FLAGS_HDTV_720p) != 0;
    bool has480p = (vidFlags & XC_VIDEO_FLAGS_HDTV_480p) != 0;
    bool has1080i = (vidFlags & XC_VIDEO_FLAGS_HDTV_1080i) != 0;

    if (has720p)
    {
        m.width = 1280;
        m.height = 720;
        m.presentFlags = D3DPRESENTFLAG_PROGRESSIVE | D3DPRESENTFLAG_WIDESCREEN;
        StrCopy(g_videoModeStr, sizeof(g_videoModeStr), "720p");
        g_isHD = true;
    }
    else if (has1080i)
    {
        m.width = 1920;
        m.height = 1080;
        m.presentFlags = D3DPRESENTFLAG_INTERLACED | D3DPRESENTFLAG_WIDESCREEN;
        StrCopy(g_videoModeStr, sizeof(g_videoModeStr), "1080i");
        g_isHD = true;
    }
    else if (has480p)
    {
        m.width = 640;
        m.height = 480;
        m.presentFlags = D3DPRESENTFLAG_PROGRESSIVE;
        StrCopy(g_videoModeStr, sizeof(g_videoModeStr), "480p");
        g_isHD = true;
    }
    else if (isTruePAL50)
    {
        // Standard PAL-I 576i — 50Hz interlaced, 4:3 backbuffer.
        // Width = 640 (not 720): 720×576 is widescreen/anamorphic PAL.
        // Standard 4:3 PAL on OG Xbox is 640×576, giving a 1:1 pixel
        // mapping in design space horizontally (g_sx = 1.0) with a small
        // vertical stretch (g_sy = 576/480 = 1.2) for the extra scan lines.
        // D3DPRESENTFLAG_INTERLACED is required — without it the NV2A
        // does not field-blend correctly and output looks wrong on PAL TVs.
        m.width = 640;
        m.height = 576;
        m.presentFlags = D3DPRESENTFLAG_INTERLACED;
        m.refreshHz = 50;
        StrCopy(g_videoModeStr, sizeof(g_videoModeStr), "576i PAL");
        g_isHD = false;
        g_isTrueInterlaced = true;  // 576i PAL50 — scanline safety on
    }
    else
    {
        // 480i baseline — NTSC, PAL-M, and PAL60 all land here.
        // NTSC 480i is true interlaced; PAL60 and PAL-M run at 60Hz like NTSC
        // so they do NOT get the scanline-safety treatment.
        m.width = 640;
        m.height = 480;
        m.presentFlags = D3DPRESENTFLAG_INTERLACED;
        if (isPAL_I && hasPAL60)
            StrCopy(g_videoModeStr, sizeof(g_videoModeStr), "480i PAL60");
        else if (isPAL_M)
            StrCopy(g_videoModeStr, sizeof(g_videoModeStr), "480i PAL-M");
        else
        {
            StrCopy(g_videoModeStr, sizeof(g_videoModeStr), "480i");
            g_isTrueInterlaced = true;  // NTSC 480i only
        }
        g_isHD = false;
    }

    // g_sx/g_sy scale 640×480 design coordinates to actual backbuffer pixels.
    g_sx = (float)m.width / 640.0f;
    g_sy = (float)m.height / 480.0f;

    return m;
}

// ============================================================================
// D3D initialisation
// ============================================================================

static bool InitD3D()
{
    VideoMode vm = DetectVideoMode();

    g_pD3D = Direct3DCreate8(D3D_SDK_VERSION);
    if (!g_pD3D) return false;

    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof(pp));

    pp.BackBufferWidth = vm.width;
    pp.BackBufferHeight = vm.height;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferCount = 1;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.Flags = vm.presentFlags;
    pp.FullScreen_RefreshRateInHz = vm.refreshHz;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    HRESULT hr = g_pD3D->CreateDevice(
        0, D3DDEVTYPE_HAL, NULL,
        D3DCREATE_HARDWARE_VERTEXPROCESSING,
        &pp, &g_pDevice);

    if (FAILED(hr))
    {
        g_pD3D->Release();
        g_pD3D = NULL;
        return false;
    }

    // Default render states
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    return true;
}

// ============================================================================
// Shutdown
// ============================================================================

static void Shutdown()
{
    if (g_logo.tex) { g_logo.tex->Release(); g_logo.tex = NULL; }
    if (g_pDevice) { g_pDevice->Release();  g_pDevice = NULL; }
    if (g_pD3D) { g_pD3D->Release();     g_pD3D = NULL; }
}

// ============================================================================
// Exit to dashboard
// ============================================================================

static void ExitToDashboard()
{
    Shutdown();
    // Launch the Xbox dashboard
    XLaunchNewImage(NULL, NULL);
    // Should not return, but idle just in case
    while (true) {}
}

// ============================================================================
// void_main
// ============================================================================

void __cdecl main()
{
    if (!InitD3D()) return;

    // Tell the font engine whether we're on a true interlaced SD display.
    // Only 480i NTSC and 576i PAL50 qualify — PAL60 and PAL-M run at 60Hz
    // and don't suffer the same scanline flicker, so they stay at HD advance.
    // Must be called after InitD3D() — that's where g_isTrueInterlaced is set.
    Font_SetSD(g_isTrueInterlaced);

    // Note: CRT overscan compensation is baked into DiagCommon.h constants.
    // BOT_BAR_H=64 / BOT_BAR_Y=416 places bottom bar text within the
    // XDK-specified 7.5% title safe boundary on all CRT displays.

    InitInput();

    // Detect and initialise physical LCD display (if fitted at SMBus 0x3C).
    // Shows splash immediately — SysInfo data fed in later via LCD_SetData.
    LCD_Begin();

    // Load the shared logo once
    g_logo.tex = DiagLoadDDS("D:\\tex\\xb.dds", g_logo.w, g_logo.h);
    // Null tex is handled gracefully by DrawLogo / DrawPageChrome

    // ---- Autorun: check for XbDiag.set --------------------------------
    g_autoSettingsFound = XbSet_LoadSettings();
    if (g_autoSettingsFound)
    {
        DWORD countStart = GetTickCount();
        bool  cancelled = false;
        WORD  prevBtns = GetButtons();
        while ((GetTickCount() - countStart) < 5000 && !cancelled)
        {
            PumpInput();
            WORD cur = GetButtons();
            if ((cur & 0x2000) && !(prevBtns & 0x2000)) cancelled = true;  // BTN_B
            prevBtns = cur;
            DWORD elapsed = GetTickCount() - countStart;
            DWORD remain = (elapsed < 5000) ? (5000 - elapsed) / 1000 + 1 : 1;
            g_pDevice->BeginScene();
            DrawPageChrome(g_logo, "XBDIAG AUTORUN", "[B] Cancel");
            float cy = CONTENT_Y + 40.f;
            DrawText(LM, cy, "AUTOMATION SETTINGS DETECTED", 1.5f,
                D3DCOLOR_XRGB(255, 220, 60)); cy += LINE_H * 2.f;
            DrawText(LM, cy, "Automated diagnostics begin in:", 1.2f,
                D3DCOLOR_XRGB(180, 180, 180)); cy += LINE_H + 4.f;
            char secBuf[8]; IntToStr((int)remain, secBuf, sizeof(secBuf));
            DrawText(LM, cy, secBuf, 4.0f, COL_CYAN); cy += LINE_H * 4.f;
            DrawText(LM, cy, "Press [B] to cancel", 1.2f, COL_DIM);
            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);
            WORD cur2 = GetButtons();
            LCD_Tick(cur2, g_lcdPrevBtns);
            g_lcdPrevBtns = cur2;
        }
        if (!cancelled)
            XbSet_AutoRun(g_logo);
    }

    // ---- Boot-time update check ----------------------------------------
    // Kick off the background version check immediately after autorun.
    // The result is polled below before entering the main game loop.
    ScreenCalib_Init(g_logo);
    if (ScreenCalib_NeedsRun())
        ScreenCalib_Run(g_logo);

    Update_StartBootCheck();
    HttpRptSrv_Start();
    HttpCap_Init();

    // Poll the update check while rendering a holding frame.
    // The check is fast (DNS + one TCP round trip) but non-blocking,
    // so we drive it to completion here before the first menu frame.
    {
        DWORD checkStart = GetTickCount();
        while (!Update_IsCheckComplete())
        {
            // Keep the screen alive with a minimal present so the watchdog
            // doesn't trip on hardware with strict frame deadlines.
            if (GetTickCount() - checkStart > 8000) break;  // hard safety cap
            PumpInput();
            g_pDevice->BeginScene();
            DrawPageChrome(g_logo, "XbDiag", "Checking for updates...");
            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);
            WORD cur3 = GetButtons();
            LCD_Tick(cur3, g_lcdPrevBtns);
            g_lcdPrevBtns = cur3;
        }
    }

    // Initial state — jump straight to Update screen if a newer version was found
    g_state = Update_BootFoundUpdate() ? STATE_UPDATE : STATE_MENU;
    g_prevState = STATE_MENU;

    // Network is confirmed up — start X-Volt discovery now.
    // Starting here rather than before the update loop ensures XNetGetTitleXnAddr
    // has resolved and the stack is ready to receive UDP broadcasts from the LAN.
    XVoltUdp_Start();

    // Notify the menu it's the first entry
    DiagMenu_OnEnter();

    while (true)
    {
        PumpInput();

        // Detect state transitions so modules can run OnEnter init
        if (g_state != g_prevState)
        {
            switch (g_state)
            {
            case STATE_MENU:    DiagMenu_OnEnter();    break;
            case STATE_SYSINFO: SysInfo_OnEnter();     break;
            case STATE_RAM:     RamTest_OnEnter();     break;
            case STATE_SMBUS:   SmBusScan_OnEnter();   break;
            case STATE_TEMP:    TempMonitor_OnEnter(); break;
            case STATE_EEPROM:  EepromView_OnEnter();  break;
            case STATE_VIDEO:   VideoInfo_OnEnter();   break;
            case STATE_HDD:     HddInfo_OnEnter();     break;
            case STATE_ABOUT:   AboutScreen_OnEnter();    break;
            case STATE_CTRL:    ControllerTest_OnEnter(); break;
            case STATE_STRESS:  StressTest_OnEnter();     break;
            case STATE_FILES:   FileExplorer_OnEnter();   break;
            case STATE_XBSET:   XbSet_OnEnter();          break;
            case STATE_UPDATE:  Update_OnEnter();          break;
            case STATE_EXIT:    ExitToDashboard();     break;
            default:            break;
            }
            g_prevState = g_state;
        }

        // Per-frame tick + render
        // Blocking modules (RamTest) own their own inner loop and call
        // RequestState(STATE_MENU) when done, so they only Tick once here.
        switch (g_state)
        {
        case STATE_MENU:    DiagMenu_Tick(g_logo);    break;
        case STATE_SYSINFO: SysInfo_Tick(g_logo);
            // Feed SysInfo data to LCD once it has finished loading (first tick)
            if (!g_lcdDataFed && LCD_IsPresent())
            {
                LCDData ld;
                SysInfo_GetLCDData(ld);
                if (ld.boardRev && ld.boardRev[0] != '\0')
                {
                    LCD_SetData(ld);
                    g_lcdDataFed = true;
                }
            }
            break;
        case STATE_RAM:     RamTest_Tick(g_logo);     break;
        case STATE_SMBUS:   SmBusScan_Tick(g_logo);   break;
        case STATE_TEMP:    TempMonitor_Tick(g_logo); break;
        case STATE_EEPROM:  EepromView_Tick(g_logo);  break;
        case STATE_VIDEO:   VideoInfo_Tick(g_logo);   break;
        case STATE_HDD:     HddInfo_Tick(g_logo);     break;
        case STATE_ABOUT:   AboutScreen_Tick(g_logo);    break;
        case STATE_CTRL:    ControllerTest_Tick(g_logo); break;
        case STATE_STRESS:  StressTest_Tick(g_logo);     break;
        case STATE_FILES:   FileExplorer_Tick(g_logo);   break;
        case STATE_XBSET:   XbSet_Tick(g_logo);          break;
        case STATE_UPDATE:  Update_Tick(g_logo);          break;
        default:            break;
        }

        // LCD tick runs every frame regardless of app state
        {
            WORD cur = GetButtons();
            LCD_Tick(cur, g_lcdPrevBtns);
            g_lcdPrevBtns = cur;
        }

        // FTP server tick runs every frame regardless of app state.
        // FtpServ_Tick is a no-op when g_ftp.state == FTP_OFF, so this
        // costs nothing when the server is not running. Keeping it here
        // ensures the listen socket accepts connections and keepalive NOOPs
        // are serviced even when the user has navigated away from FileExplorer.
        FtpServ_Tick();
        HttpCap_CaptureFrame();
        HttpRptSrv_Poll();
        XVoltUdp_Poll();
    }
}