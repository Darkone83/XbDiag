// AboutScreen.cpp
// XbDiag - About screen
//
// Layout:
//   Top section (CONTENT_Y to DIVIDER_Y):
//     App name + version  (large, centered)
//     Purpose statement   (two lines, centered)
//     Horizontal rule
//     Target hardware row + modules rows (three lines)
//     Special thanks label + rainbow wave scroller
//
//   Lower panel (DIVIDER_Y to BOT_BAR_Y):
//     Two credit cards side by side, centered:
//       Left  - tr.dds   "Team Resurgent"
//       Right - dc.dds   "Darkone Customs"
//     Logos loaded on OnEnter, released on [B] back.
//     Rotating Xbox trivia ticker above the bot bar.

#include "AboutScreen.h"
#include "Update.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

// ============================================================================
// Credit logo state
// ============================================================================

static DiagLogo s_trLogo;   // Team Resurgent   - tr.dds
static DiagLogo s_dcLogo;   // Darkone Customs  - dc.dds

static WORD s_prevBtns = 0;

// ============================================================================
// Xbox facts + animation state
// ============================================================================

static const char* s_facts[] =
{
    "The original Xbox was the first console to ship with a built-in hard drive.",
    "Bill Gates and The Rock unveiled the Xbox together at CES 2001.",
    "The Xbox CPU is a custom 733 MHz Pentium III - codenamed 'Coppermine-T'.",
    "The NV2A GPU was co-developed by Microsoft and NVIDIA exclusively for Xbox.",
    "Xbox was the first console to feature built-in Ethernet as standard.",
    "The Xbox BIOS verifies its own SHA-1 hash before handing off to the bootloader.",
    "A debug Xbox (green shell) has 128MB RAM - double the retail 64MB.",
    "The Xbox HDD is locked to the console via a key stored in the EEPROM.",
    "The original Xbox dashboard was codenamed 'Mecca' during development.",
    "Xbox used a modified Intel southbridge nicknamed the 'MCP-X'.",
    "Xbox Live launched November 15, 2002 - exactly one year after the console.",
    "The 1.6 board revision replaced the SMBus video encoder with the Xcalibur ASIC.",
    "An Xbox power supply outputs 5V, 12V, and 3.3V rails simultaneously.",
    "The Xbox kernel is a stripped-down Windows 2000 kernel with a custom HAL.",
    "Cromwell is an open-source replacement BIOS for the original Xbox.",
    "TSOP flash on boards 1.0-1.5 can be reflashed without a modchip.",
    "The original Xbox codename during development was 'DirectX Box'.",
    "Xbox retail units shipped with Thomson, Samsung, Philips, or Hitachi DVD drives.",
    "The NV2A GPU runs at 233 MHz on a shared 128-bit memory bus with the CPU.",
    "A 1.6 Xbox cannot be TSOP-flashed - the flash chip is not wired to the bus.",
    "Softmodding typically exploits a buffer overflow in a saved game file.",
    "Xbox EEPROM stores the serial number, region, HDD key, and MAC address.",
    "Xbox Live originally required a subscription of $49.99 per year at launch.",
    "There were over 900 Xbox games released during its commercial lifespan.",
};
static const int NUM_FACTS = sizeof(s_facts) / sizeof(s_facts[0]);

static int   s_factIdx = 0;
static DWORD s_factStart = 0;
static float s_factAlpha = 0.f;
static bool  s_factFadingIn = true;

static const DWORD FACT_HOLD_MS = 4000;
static const DWORD FACT_FADE_MS = 500;

// ============================================================================
// Credits scroller state
// ============================================================================

static const char k_scrollText[] =
"LuckyXmods -=- Equinox -=- Andr0 -=- Haguero -=- Harcroft -=- "
"Wolfgang von Douchenozzle -=- Bomb Bloke -=- The XBOX-Scene Discord -=- "
"The OG Xbox Community      ";

static float s_scrollX = 0.f;   // current left edge of scroller text (pixels)
static float s_scrollTime = 0.f;   // accumulated time for wave + RGB phase (seconds)
static DWORD s_scrollPrev = 0;     // GetTickCount at last scroller tick
static bool  s_scrollInit = false;

static float Clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

static void TickFact()
{
    DWORD now = GetTickCount();
    DWORD elapsed = now - s_factStart;

    if (s_factFadingIn)
    {
        s_factAlpha = Clamp01((float)elapsed / (float)FACT_FADE_MS);
        if (elapsed >= FACT_FADE_MS) s_factFadingIn = false;
    }
    else
    {
        DWORD holdEnd = FACT_FADE_MS + FACT_HOLD_MS;
        if (elapsed < holdEnd)
        {
            s_factAlpha = 1.f;
        }
        else
        {
            DWORD fadeOut = elapsed - holdEnd;
            s_factAlpha = Clamp01(1.f - (float)fadeOut / (float)FACT_FADE_MS);
            if (fadeOut >= FACT_FADE_MS)
            {
                s_factIdx = (s_factIdx + 1) % NUM_FACTS;
                s_factStart = now;
                s_factAlpha = 0.f;
                s_factFadingIn = true;
            }
        }
    }
}

// ============================================================================
// Layout constants
// ============================================================================

static const float DIVIDER_Y = 260.f;   // splits info section from logo panel
static const float LOGO_DISP_W = 128.f;   // display size of each credit logo
static const float LOGO_DISP_H = 64.f;
static const float CARD_W = 220.f;
static const float CARD_H = 110.f;
static const float CARD_GAP = 40.f;

// ============================================================================
// OnEnter  -  load credit logos
// ============================================================================

void AboutScreen_OnEnter()
{
    s_prevBtns = 0;
    s_factIdx = 0;
    s_factStart = GetTickCount();
    s_factAlpha = 0.f;
    s_factFadingIn = true;

    if (s_trLogo.tex) { s_trLogo.tex->Release(); s_trLogo.tex = NULL; }
    if (s_dcLogo.tex) { s_dcLogo.tex->Release(); s_dcLogo.tex = NULL; }

    s_trLogo.tex = DiagLoadDDS("D:\\tex\\tr.dds", s_trLogo.w, s_trLogo.h);
    s_dcLogo.tex = DiagLoadDDS("D:\\tex\\dc.dds", s_dcLogo.w, s_dcLogo.h);

    s_scrollX = (float)SW;
    s_scrollTime = 0.f;
    s_scrollInit = false;
}

// ============================================================================
// Input helper
// ============================================================================

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

// ============================================================================
// DrawCreditCard
// Draws a bordered card with logo centered above name text.
// tagline is accepted but currently passed as "" at all call sites.
// ============================================================================

static void DrawCreditCard(float cx, float cy,
    const DiagLogo& cardLogo,
    const char* name,
    const char* tagline)
{
    float x0 = cx - CARD_W * 0.5f;
    float y0 = cy - CARD_H * 0.5f;
    float x1 = cx + CARD_W * 0.5f;
    float y1 = cy + CARD_H * 0.5f;

    // Card background
    FillRectGrad(x0, y0, x1, y1,
        D3DCOLOR_XRGB(20, 30, 65),
        D3DCOLOR_XRGB(12, 18, 42));

    // Card border
    HLine(y0, x0, x1, COL_BORDER);
    HLine(y1, x0, x1, COL_BORDER);
    VLine(x0, y0, y1, COL_BORDER);
    VLine(x1 - 1.f, y0, y1, COL_BORDER);

    // Logo + name block centered vertically within the card
    // Block = LOGO_DISP_H + 6px gap + LINE_H text
    const float NAME_S = 1.4f;
    const float BLOCK_H = LOGO_DISP_H + 6.f + LINE_H;
    float blockTop = cy - BLOCK_H * 0.5f;
    float logoCY = blockTop + LOGO_DISP_H * 0.5f;
    DrawLogo(cardLogo, cx, logoCY, LOGO_DISP_W, LOGO_DISP_H, 255);
    DiagResetShader();

    // Name centered below logo
    float nameY = blockTop + LOGO_DISP_H + 6.f;
    DrawText(cx - TW(name, NAME_S) * 0.5f, nameY, name, NAME_S, COL_WHITE);

    // Tagline below name
    const float TAG_S = 1.1f;
    float tagY = nameY + LINE_H;
    DrawText(cx - TW(tagline, TAG_S) * 0.5f, tagY, tagline, TAG_S, COL_DIM);
}

// ============================================================================
// TickScroller
//
// Advances scroll position and time accumulator each frame.
// s_scrollX starts at SW (right edge) on first tick and moves left at
// SCROLL_SPEED px/sec. When the entire text clears the left edge it resets
// to SW for seamless wrap.
//
// Constants below are shared with DrawScroller.
// ============================================================================

static const float SCROLL_SCALE = 1.3f;   // glyph scale for scroller text
static const float SCROLL_SPEED = 60.f;   // pixels per second
static const float SCROLL_AMP = 7.f;    // wave amplitude in pixels
static const float SCROLL_WFREQ = 0.018f; // spatial wave frequency (rad/px)
static const float SCROLL_TFREQ = 2.2f;   // temporal wave frequency (rad/s)

static void TickScroller()
{
    DWORD now = GetTickCount();
    if (!s_scrollInit)
    {
        s_scrollX = (float)SW;
        s_scrollPrev = now;
        s_scrollInit = true;
    }

    DWORD elapsed = now - s_scrollPrev;
    s_scrollPrev = now;

    float dt = (float)elapsed * 0.001f;
    if (dt > 0.1f) dt = 0.1f;   // clamp on first frame / stall

    s_scrollTime += dt;

    // Advance text left
    s_scrollX -= SCROLL_SPEED * dt;

    // Measure total text width so we can wrap cleanly
    int   len = 0; while (k_scrollText[len]) ++len;
    float textW = (float)len * Font_GetAdvance() * SCROLL_SCALE;

    // Once the tail has passed the left edge, reset to right
    if (s_scrollX + textW < 0.f)
        s_scrollX = (float)SW;
}

// ============================================================================
// DrawScroller
//
// Draws each character at its own Y offset (sine wave) with a rainbow colour
// that shifts per character position and advances over time.
// Each character is drawn as a single DrawText call with a one-char string.
// ============================================================================

static void DrawScroller(float baseY)
{
    float advance = Font_GetAdvance() * SCROLL_SCALE;
    float cx = s_scrollX;

    char buf[2];
    buf[1] = '\0';

    int len = 0; while (k_scrollText[len]) ++len;

    for (int i = 0; i < len; ++i, cx += advance)
    {
        // Cull characters fully off-screen
        if (cx + advance < 0.f || cx >(float)SW) continue;

        // Wave: Y offset = amplitude * sin(spatial_phase + temporal_phase)
        float xPhase = cx * SCROLL_WFREQ;
        float waveY = SCROLL_AMP * sinf(xPhase + s_scrollTime * SCROLL_TFREQ);

        // Rainbow: hue cycles with character index + time
        // H in [0,1], full saturation, full value -> RGB
        float hue = (float)i * (1.f / 24.f) + s_scrollTime * 0.18f;
        hue = hue - (float)Ftoi(hue);   // frac(), no __ftol2_sse

        // HSV->RGB: S=1, V=1
        float h6 = hue * 6.f;
        int   hi = Ftoi(h6);
        float f = h6 - (float)hi;
        // p=0, q=1-f, t=f  (S=V=1 simplification)
        float r = 0.f, g = 0.f, b = 0.f;
        switch (hi % 6)
        {
        case 0: r = 1.f;       g = f;         b = 0.f;       break;
        case 1: r = 1.f - f;   g = 1.f;       b = 0.f;       break;
        case 2: r = 0.f;       g = 1.f;       b = f;         break;
        case 3: r = 0.f;       g = 1.f - f;   b = 1.f;       break;
        case 4: r = f;         g = 0.f;       b = 1.f;       break;
        case 5: r = 1.f;       g = 0.f;       b = 1.f - f;   break;
        }

        BYTE rb = (BYTE)Ftoi(r * 255.f);
        BYTE gb = (BYTE)Ftoi(g * 255.f);
        BYTE bb = (BYTE)Ftoi(b * 255.f);
        DWORD col = D3DCOLOR_XRGB(rb, gb, bb);

        buf[0] = k_scrollText[i];
        DrawText(cx, baseY + waveY, buf, SCROLL_SCALE, col);
    }
}

// ============================================================================
// Render
// ============================================================================

static void Render(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    DrawPageChrome(logo, "ABOUT  XbDiag", "[X/Y] Cycle Facts    [B] Back to Menu");

    float y = CONTENT_Y + 6.f;

    // -------------------------------------------------------------------------
    // Title + version
    // -------------------------------------------------------------------------
    const float TITLE_S = 2.2f;
    const float VER_S = 1.5f;
    const char* k_title = "XbDiag";
    const char* k_ver = Update_GetLocalVersion();

    float titleW = TW(k_title, TITLE_S);
    float blockW = titleW + 14.f + TW(k_ver, VER_S);
    float titleX = (SW - blockW) * 0.5f;

    DrawText(titleX, y, k_title, TITLE_S, COL_CYAN);
    DrawText(titleX + titleW + 14.f,
        y + 7.f * TITLE_S - 7.f * VER_S,
        k_ver, VER_S, COL_YELLOW);

    y += 7.f * TITLE_S + 8.f;

    // -------------------------------------------------------------------------
    // Purpose  (two lines, centered)
    // -------------------------------------------------------------------------
    const char* k_p1 = "A hardware diagnostic suite for the original Xbox console.";
    const char* k_p2 = "Inspect RAM, SMBus, EEPROM, temperatures, video and storage.";

    DrawTextC(g_marginL + (SW - g_marginL - g_marginR) * 0.5f, y, k_p1, 1.3f, COL_GRAY);
    y += LINE_H + 2.f;
    DrawTextC(g_marginL + (SW - g_marginL - g_marginR) * 0.5f, y, k_p2, 1.2f, COL_DIM);
    y += LINE_H + GROUP_GAP;

    // -------------------------------------------------------------------------
    // Rule
    // -------------------------------------------------------------------------
    HLine(y, g_marginL, SW - g_marginR, COL_BORDER);
    y += 8.f;

    // -------------------------------------------------------------------------
    // Target row only
    // -------------------------------------------------------------------------
    const float LS = 1.3f;

    DrawText(g_marginL, y, "TARGET HARDWARE :", LS, COL_GRAY);
    DrawText(g_marginL + 162.f, y, "Original Xbox  Rev 1.0 - 1.6  |  Debug kit", LS, COL_WHITE);
    y += LINE_H;

    DrawText(g_marginL, y, "MODULES         :", LS, COL_GRAY);
    DrawText(g_marginL + 162.f, y, "SysInfo  RAM Test  SMBus Scan  Temp Monitor", LS, COL_WHITE);
    y += LINE_H;
    DrawText(g_marginL + 162.f, y, "HDD Info  EEPROM  Video Out  Stress Test", LS, COL_WHITE);
    y += LINE_H;
    DrawText(g_marginL + 162.f, y, "File Explorer  Controller Test  Update Check", LS, COL_WHITE);
    y += LINE_H + 6.f;

    // -------------------------------------------------------------------------
    // Special thanks label + wave scroller
    // Lives in the natural gap between the modules list and DIVIDER_Y.
    // -------------------------------------------------------------------------
    {
        const char* k_thanks = "Special Thanks To:";
        const float LABEL_S = 1.1f;
        DrawTextC(g_marginL + (SW - g_marginL - g_marginR) * 0.5f, y, k_thanks, LABEL_S, COL_WHITE);
        y += LINE_H + 2.f;
        DrawScroller(y);
    }

    // -------------------------------------------------------------------------
    // Divider + logo panel background
    // -------------------------------------------------------------------------
    HLine(DIVIDER_Y, 0.f, SW, COL_BORDER);

    float panelTop = DIVIDER_Y + 1.f;
    float panelBot = BOT_BAR_Y - 1.f;

    FillRectGrad(0.f, panelTop, SW, panelBot,
        D3DCOLOR_XRGB(12, 16, 36),
        D3DCOLOR_XRGB(8, 10, 24));

    // -------------------------------------------------------------------------
    // Credit cards  -  centered in panel
    // -------------------------------------------------------------------------
    float panelCY = panelTop + (panelBot - panelTop) * 0.5f;
    float totalCardsW = CARD_W * 2.f + CARD_GAP;
    float leftCX = g_marginL + (SW - g_marginL - g_marginR - totalCardsW) * 0.5f + CARD_W * 0.5f;
    float rightCX = leftCX + CARD_W + CARD_GAP;

    DrawCreditCard(leftCX, panelCY, s_trLogo,
        "Team Resurgent", "");

    DrawCreditCard(rightCX, panelCY, s_dcLogo,
        "Darkone Customs", "");

    // -------------------------------------------------------------------------
    // Fact ticker  -  centered in gap between card bottom and panel bottom
    // -------------------------------------------------------------------------
    {
        float cardBot = panelCY + CARD_H * 0.5f;
        float tickerY = cardBot + (panelBot - cardBot) * 0.5f - 7.f;
        BYTE alpha = Ftoi(s_factAlpha * 220.f);
        if (alpha > 0)
        {
            DWORD fc = (((DWORD)alpha) << 24) | 0x00B8CCD8;
            const char* fact = s_facts[s_factIdx];
            float maxW = SW - LM * 2.f;

            if (TW(fact, 1.15f) <= maxW)
            {
                DrawTextC(g_marginL + (SW - g_marginL - g_marginR) * 0.5f, tickerY, fact, 1.15f, fc);
            }
            else
            {
                int len = 0; while (fact[len]) ++len;
                int split = len / 2;
                while (split > 0 && fact[split] != ' ') --split;

                char line1[128];
                int n = split < 127 ? split : 127;
                for (int i = 0; i < n; ++i) line1[i] = fact[i];
                line1[n] = '\0';
                const char* line2 = fact + split + 1;

                DrawTextC(g_marginL + (SW - g_marginL - g_marginR) * 0.5f, tickerY - 7.f, line1, 1.1f, fc);
                DrawTextC(g_marginL + (SW - g_marginL - g_marginR) * 0.5f, tickerY + 7.f, line2, 1.1f, fc);
            }
        }
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// Tick
// ============================================================================

void AboutScreen_Tick(const DiagLogo& logo)
{
    WORD cur = GetButtons();

    if (EdgeDown(cur, s_prevBtns, BTN_B) ||
        EdgeDown(cur, s_prevBtns, BTN_BACK))
    {
        if (s_trLogo.tex) { s_trLogo.tex->Release(); s_trLogo.tex = NULL; }
        if (s_dcLogo.tex) { s_dcLogo.tex->Release(); s_dcLogo.tex = NULL; }
        RequestState(MSTATE_MENU);
    }

    if (EdgeDown(cur, s_prevBtns, BTN_X))
    {
        s_factIdx = (s_factIdx + NUM_FACTS - 1) % NUM_FACTS;
        s_factStart = GetTickCount(); s_factAlpha = 0.f; s_factFadingIn = true;
    }
    if (EdgeDown(cur, s_prevBtns, BTN_Y))
    {
        s_factIdx = (s_factIdx + 1) % NUM_FACTS;
        s_factStart = GetTickCount(); s_factAlpha = 0.f; s_factFadingIn = true;
    }

    s_prevBtns = cur;
    TickFact();
    TickScroller();
    Render(logo);
}