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
//   STATE_ABOUT    -> AboutScreen (static display)
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
#include "XbSet.h"

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
    STATE_EXIT,
};

static AppState g_state = STATE_MENU;
static AppState g_prevState = STATE_MENU;

// Called by any module to return to the main menu
void RequestState(int newState)
{
    g_state = (AppState)newState;
}

// ============================================================================
// D3D initialisation
// ============================================================================

// ============================================================================
// Video mode detection
// Reads XGetVideoFlags() and XGetVideoStandard() to pick the best available
// mode.  Priority: 720p > 480p > 576i (PAL) > 480i (NTSC fallback).
// Sets g_sx, g_sy, g_isHD, g_videoModeStr before D3D init.
// SW/SH remain fixed at 640/480 design units - only g_sx/g_sy know the real res.
// Returns the present flags and backbuffer dims to use in CreateDevice.
// ============================================================================

struct VideoMode
{
    DWORD width;
    DWORD height;
    DWORD presentFlags;   // D3DPRESENTFLAG_* combination
    DWORD refreshHz;
};

static VideoMode DetectVideoMode()
{
    DWORD vidStd = XGetVideoStandard();
    DWORD vidFlags = XGetVideoFlags();

    VideoMode m;
    m.refreshHz = 60;

    bool isPAL = (vidStd == XC_VIDEO_STANDARD_PAL_I);
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
        // 1080i - use 720p res since most diag content is simpler
        // and 1080i is rarely useful for a diag tool; keep as fallback
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
    else if (isPAL)
    {
        m.width = 720;
        m.height = 576;
        m.presentFlags = 0;
        m.refreshHz = 50;
        StrCopy(g_videoModeStr, sizeof(g_videoModeStr), "576i PAL");
        g_isHD = false;
    }
    else
    {
        // NTSC 480i - baseline fallback
        m.width = 640;
        m.height = 480;
        m.presentFlags = D3DPRESENTFLAG_INTERLACED;
        StrCopy(g_videoModeStr, sizeof(g_videoModeStr), "480i");
        g_isHD = false;
    }

    // Update runtime layout globals
    // SW/SH are compile-time constants (640/480 design space).
    // g_sx/g_sy scale those design coords to the actual backbuffer at draw time.
    g_sx = (float)m.width / 640.0f;
    g_sy = (float)m.height / 480.0f;

    return m;
}

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

    InitInput();

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
            if ((cur & 0x2000) && !(prevBtns & 0x2000)) cancelled = true;
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
        }
        if (!cancelled)
            XbSet_AutoRun(g_logo);
    }

    // Initial state
    g_state = STATE_MENU;
    g_prevState = STATE_MENU;

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
        case STATE_SYSINFO: SysInfo_Tick(g_logo);     break;
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
        default:            break;
        }
    }
}