// DiagMenu.cpp
// XbDiag main navigation menu.
//
// Layout:
//   Chrome:  DrawPageChrome (top bar, bottom bar, logo)
//   Content: Vertically centered list of 11 menu entries.
//            Selected row = full-width FillRectGrad highlight bar (COL_SEL_BAR).
//            Each row: index number  |  label  |  one-line description  |  status badge
//
// Navigation:
//   D-pad up/down  = move cursor
//   A              = enter selected module
//   Start          = exit to dashboard
//   Back+White     = open automation settings (hidden)
//
// Status badges:
//   "READY"     COL_GREEN  - module available
//   (reserved for future "N/A" states on specific hardware)

#include "DiagMenu.h"
#include "font.h"
#include "input.h"
#include "Update.h"

#include <xtl.h>

// App-level state request (defined in main.cpp)
extern void RequestState(int newState);

// AppState enum values we need (keep in sync with main.cpp)
enum
{
    MSTATE_MENU = 0,
    MSTATE_SYSINFO = 1,
    MSTATE_RAM = 2,
    MSTATE_SMBUS = 3,
    MSTATE_TEMP = 4,
    MSTATE_EEPROM = 5,
    MSTATE_VIDEO = 6,
    MSTATE_HDD = 7,
    MSTATE_ABOUT = 8,
    MSTATE_CTRL = 9,
    MSTATE_STRESS = 10,
    MSTATE_FILES = 11,
    MSTATE_XBSET = 12,
    MSTATE_UPDATE = 13,
    MSTATE_EXIT = 14,
};

// ============================================================================
// Menu entry definition
// ============================================================================

struct MenuItem
{
    const char* index;       // "01" .. "08"
    const char* label;       // short name, uppercase
    const char* desc;        // one-line description shown to the right
    int         targetState; // AppState to transition to on [A]
};

static const MenuItem k_items[] =
{
    { "01", "SYSTEM INFO",      "Full hardware snapshot - CPU, RAM, video, storage, thermals",  MSTATE_SYSINFO },
    { "02", "MEMORY TEST",      "Walk test free RAM banks, detect bad pages and report errors", MSTATE_RAM     },
    { "03", "SMBUS SCAN",       "Scan all SMBus addresses, decode known devices and registers",  MSTATE_SMBUS   },
    { "04", "TEMP MONITOR",     "Live CPU and board temperature via ADM1032 / PIC registers",   MSTATE_TEMP    },
    { "05", "EEPROM VIEWER",    "Read and decode EEPROM - serial, region, keys, MAC address",   MSTATE_EEPROM  },
    { "06", "VIDEO INFO",       "Encoder type, AV pack, resolution, refresh rate, color depth", MSTATE_VIDEO   },
    { "07", "HDD INFO",         "ATA IDENTIFY - model, serial, capacity, UDMA mode",            MSTATE_HDD     },
    { "08", "CONTROLLER TEST",  "Buttons, analog sticks, triggers, and rumble motor test",      MSTATE_CTRL    },
    { "09", "STRESS TEST",      "Sustained CPU and RAM load test with thermal monitoring",      MSTATE_STRESS  },
    { "10", "FILE EXPLORER",    "Browse Xbox partitions - C: E: F: G: and MU slots",            MSTATE_FILES   },
    { "11", "UPDATE CHECK",     "Check GitHub for a newer version and download if available",    MSTATE_UPDATE  },
    { "12", "ABOUT",            "Version info, credits, and hardware compatibility notes",       MSTATE_ABOUT   },
};

static const int k_itemCount = sizeof(k_items) / sizeof(k_items[0]);

// ============================================================================
// State
// ============================================================================

static int  s_sel = 0;       // current selection 0..k_itemCount-1
static WORD s_prevBtns = 0;       // for edge detection

// ============================================================================
// Layout constants
// ============================================================================

static const float ROW_H = 26.f;   // compact row height
static const float ROW_PAD_Y = 6.f;    // text padding from top of row
static const float LIST_TOP = CONTENT_Y;             // flush under top bar
static const float IDX_X = 12.f;   // index number X
static const float LABEL_X = 52.f;   // label text X
static const float BADGE_RX = SW - 12.f; // READY badge right edge
static const float TS = 1.4f;   // row text scale
static const float INFO_Y = LIST_TOP + (float)12 * ROW_H + 10.f; // info panel Y

// ============================================================================
// OnEnter
// ============================================================================

void DiagMenu_OnEnter()
{
    // Preserve selection across re-entries so the cursor stays put.
    // Seed prevBtns from current held state — buttons held during the
    // transition (e.g. START from ControllerTest START+B exit chord)
    // won't fire as a fresh edge on the first Tick.
    s_prevBtns = GetButtons();
}

// ============================================================================
// Input handling
// ============================================================================

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

static void HandleInput()
{
    WORD cur = GetButtons();

    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_UP))
    {
        s_sel--;
        if (s_sel < 0) s_sel = k_itemCount - 1;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_DPAD_DOWN))
    {
        s_sel++;
        if (s_sel >= k_itemCount) s_sel = 0;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_A))
    {
        RequestState(k_items[s_sel].targetState);
    }
    if (EdgeDown(cur, s_prevBtns, BTN_START))
    {
        RequestState(MSTATE_EXIT);
    }
    // Back+White chord opens automation settings (hidden menu).
    // Both must be held; at least one must have just gone down this frame.
    {
        bool backHeld = (cur & BTN_BACK) != 0;
        bool whiteHeld = (cur & BTN_WHITE) != 0;
        bool backEdge = backHeld && !(s_prevBtns & BTN_BACK);
        bool whiteEdge = whiteHeld && !(s_prevBtns & BTN_WHITE);
        if (backHeld && whiteHeld && (backEdge || whiteEdge))
            RequestState(MSTATE_XBSET);
    }

    s_prevBtns = cur;
}

// ============================================================================
// Render
// ============================================================================

static void Render(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    DrawPageChrome(logo,
        "XbDiag v1.0.2 Beta -  Hardware Diagnostic Suite",
        "[Up/Down] Navigate    [A] Select    [Start] Exit to Dashboard");

    // -------------------------------------------------------------------------
    // List - pinned flush under top bar
    // -------------------------------------------------------------------------
    for (int i = 0; i < k_itemCount; ++i)
    {
        float rowY = LIST_TOP + (float)i * ROW_H;
        bool  sel = (i == s_sel);

        // Selection highlight bar - full width
        if (sel)
        {
            FillRectGrad(0.f, rowY, SW, rowY + ROW_H,
                COL_SEL_BAR,
                COL_SEL_BAR2);
            // Left accent stripe
            FillRect(0.f, rowY, 4.f, rowY + ROW_H,
                D3DCOLOR_XRGB(80, 140, 255));
        }

        // Row separator on every row
        HLine(rowY, 0.f, SW, D3DCOLOR_XRGB(22, 28, 55));

        float textY = rowY + ROW_PAD_Y;
        DWORD idxCol = sel ? COL_CYAN : COL_DIM;
        DWORD lblCol = sel ? COL_WHITE : COL_GRAY;

        // Index
        DrawText(IDX_X, textY, k_items[i].index, TS, idxCol);

        // Label
        DrawText(LABEL_X, textY, k_items[i].label, TS, lblCol);

        // READY badge right-aligned
        DrawTextR(BADGE_RX, textY, "READY", 1.3f, sel ? COL_GREEN : COL_DIM);
    }

    // Bottom border of list
    float listBotY = LIST_TOP + (float)k_itemCount * ROW_H;
    HLine(listBotY, 0.f, SW, COL_BORDER);

    // -------------------------------------------------------------------------
    // Info panel - description of selected item below the list
    // -------------------------------------------------------------------------
    float panelY = listBotY + 8.f;
    float panelH = BOT_BAR_Y - panelY - 18.f;

    // Panel background
    FillRectGrad(0.f, panelY, SW, panelY + panelH,
        D3DCOLOR_XRGB(14, 18, 40),
        D3DCOLOR_XRGB(10, 12, 28));

    // Panel border
    HLine(panelY, 0.f, SW, COL_BORDER);
    HLine(panelY + panelH, 0.f, SW, COL_BORDER);

    // Label header
    float infoTextY = panelY + (panelH - 7.f * 1.3f) * 0.5f;
    DrawText(IDX_X + 8.f, infoTextY, k_items[s_sel].label, 1.3f, COL_CYAN);

    // Divider between label and desc
    float divX = IDX_X + 8.f + TW(k_items[s_sel].label, 1.3f) + 10.f;
    VLine(divX, panelY + 6.f, panelY + panelH - 6.f, COL_BORDER);

    // Description
    DrawText(divX + 10.f, infoTextY, k_items[s_sel].desc, 1.2f, COL_GRAY);

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// Public tick (called every frame from main.cpp game loop)
// ============================================================================

void DiagMenu_Tick(const DiagLogo& logo)
{
    HandleInput();
    Render(logo);
}