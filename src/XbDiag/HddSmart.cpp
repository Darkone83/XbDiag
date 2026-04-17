// HddSmart.cpp
// XbDiag - HDD SMART view.
//
// Renders the SMART attribute table and exports D:\smart.txt.
// All data comes from s_data.smartBuf (populated by LoadData in HddInfo.cpp).

#include "HddSmart.h"
#include "HddInfo.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

// ============================================================================
// SMART attribute name table
// ============================================================================

static const char* SmartAttrName(BYTE id)
{
    switch (id)
    {
    case 0x01: return "Read Error Rate";
    case 0x02: return "Throughput Perf";
    case 0x03: return "Spin-Up Time";
    case 0x04: return "Start/Stop Count";
    case 0x05: return "Reallocated Sects";
    case 0x07: return "Seek Error Rate";
    case 0x08: return "Seek Time Perf";
    case 0x09: return "Power-On Hours";
    case 0x0A: return "Spin Retry Count";
    case 0x0B: return "Calibration Retry";
    case 0x0C: return "Power Cycle Count";
    case 0xAA: return "Available Reservd";
    case 0xAB: return "Program Fail Count";
    case 0xAC: return "Erase Fail Count";
    case 0xAD: return "Wear Level Count";
    case 0xAE: return "Unexpected Poweroff";
    case 0xB8: return "End-to-End Error";
    case 0xBB: return "Uncorr ECC Count";
    case 0xBC: return "Command Timeout";
    case 0xBD: return "High Fly Writes";
    case 0xBE: return "Temp Difference";
    case 0xBF: return "G-Sense Errors";
    case 0xC0: return "Unsafe Shutdowns";
    case 0xC1: return "Load/Unload Cycles";
    case 0xC2: return "Temperature";
    case 0xC3: return "Hardware ECC Recov";
    case 0xC4: return "Realloc Event Cnt";
    case 0xC5: return "Pending Sectors";
    case 0xC6: return "Uncorrectable Sects";
    case 0xC7: return "UDMA CRC Errors";
    case 0xC8: return "Write Error Rate";
    case 0xCA: return "Data Addr Mark Errs";
    case 0xCB: return "Run Out Cancel";
    case 0xF0: return "Head Flying Hours";
    case 0xF1: return "Total LBA Written";
    case 0xF2: return "Total LBA Read";
    case 0xFE: return "Free Fall Protect";
    default:   return NULL;
    }
}

static bool SmartAttrCritical(BYTE id)
{
    return (id == 0x05 || id == 0xC5 || id == 0xC6);
}

// ============================================================================
// SMART constants
// ============================================================================

#define SMART_MAX_ATTRS   30
#define SMART_ATTR_SIZE   12

// ============================================================================
// HddSmart_Export
// ============================================================================

void HddSmart_Export()
{
    HddData& d = s_data;
    d.smartExportDone = false;
    d.smartExportOK = false;

    HANDLE hf = CreateFileA("D:\\smart.txt", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        d.smartExportDone = true; return;
    }

    DWORD w;
    char line[128];

    const char* hdr = "XbDiag SMART Data\r\n==================\r\n";
    WriteFile(hf, hdr, StrLen(hdr), &w, NULL);

    StrCopy(line, sizeof(line), "Drive:    "); StrCat2(line, sizeof(line), line, d.model);
    StrCat2(line, sizeof(line), line, "\r\n"); WriteFile(hf, line, StrLen(line), &w, NULL);
    StrCopy(line, sizeof(line), "Serial:   "); StrCat2(line, sizeof(line), line, d.serial);
    StrCat2(line, sizeof(line), line, "\r\n"); WriteFile(hf, line, StrLen(line), &w, NULL);

    if (!d.smartOK)
    {
        const char* msg = "SMART: Not supported or read failed\r\n";
        WriteFile(hf, msg, StrLen(msg), &w, NULL);
        FlushFileBuffers(hf); CloseHandle(hf);
        d.smartExportDone = true; d.smartExportOK = true;
        return;
    }

    const char* col = "\r\nID    Name                 Cur  Wst  Thr  Raw\r\n"
        "----  -------------------  ---  ---  ---  ------------\r\n";
    WriteFile(hf, col, StrLen(col), &w, NULL);

    const BYTE* p = d.smartBuf + 2;
    for (int i = 0; i < SMART_MAX_ATTRS; ++i, p += SMART_ATTR_SIZE)
    {
        BYTE id = p[0];
        if (id == 0) continue;

        BYTE cur = p[3];
        BYTE wst = p[4];
        BYTE thr = p[5];
        const BYTE* raw = p + 6;

        char idStr[4];  IntToHex(id, 2, idStr, sizeof(idStr));
        char curStr[4]; IntToStr(cur, curStr, sizeof(curStr));
        char wstStr[4]; IntToStr(wst, wstStr, sizeof(wstStr));
        char thrStr[4]; IntToStr(thr, thrStr, sizeof(thrStr));

        char rawStr[20]; rawStr[0] = '\0';
        char rb[4];
        for (int j = 5; j >= 0; --j)
        {
            IntToHex(raw[j], 2, rb, sizeof(rb));
            StrCat2(rawStr, sizeof(rawStr), rawStr, rb);
            if (j > 0) StrCat2(rawStr, sizeof(rawStr), rawStr, " ");
        }

        const char* name = SmartAttrName(id);
        char nameBuf[22];
        if (name) StrCopy(nameBuf, sizeof(nameBuf), name);
        else { StrCopy(nameBuf, sizeof(nameBuf), "Attr 0x"); StrCat2(nameBuf, sizeof(nameBuf), nameBuf, idStr); }
        int nl = (int)StrLen(nameBuf);
        while (nl < 20) { nameBuf[nl++] = ' '; }
        nameBuf[20] = '\0';

        char cStr[5]; StrCopy(cStr, sizeof(cStr), cur < 100 ? (cur < 10 ? "  " : " ") : "");
        StrCat2(cStr, sizeof(cStr), cStr, curStr);
        char wStr[5]; StrCopy(wStr, sizeof(wStr), wst < 100 ? (wst < 10 ? "  " : " ") : "");
        StrCat2(wStr, sizeof(wStr), wStr, wstStr);
        char tStr[5]; StrCopy(tStr, sizeof(tStr), thr < 100 ? (thr < 10 ? "  " : " ") : "");
        StrCat2(tStr, sizeof(tStr), tStr, thrStr);

        StrCopy(line, sizeof(line), idStr);
        StrCat2(line, sizeof(line), line, "    ");
        StrCat2(line, sizeof(line), line, nameBuf);
        StrCat2(line, sizeof(line), line, "  ");
        StrCat2(line, sizeof(line), line, cStr);
        StrCat2(line, sizeof(line), line, "  ");
        StrCat2(line, sizeof(line), line, wStr);
        StrCat2(line, sizeof(line), line, "  ");
        StrCat2(line, sizeof(line), line, tStr);
        StrCat2(line, sizeof(line), line, "  ");
        StrCat2(line, sizeof(line), line, rawStr);
        StrCat2(line, sizeof(line), line, "\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);
    }

    FlushFileBuffers(hf);
    CloseHandle(hf);
    d.smartExportDone = true;
    d.smartExportOK = true;
}

// ============================================================================
// Scroll state -- file scope, reset when entering the view
// ============================================================================

struct SmartRow { BYTE id; BYTE cur; BYTE wst; BYTE thr; BYTE raw[6]; };
static SmartRow s_smartRows[SMART_MAX_ATTRS];

static int  s_smartScroll = 0;
static bool s_smartScrollRst = true;
static WORD s_smartPrevBtns = 0;

void HddSmart_ResetScroll() { s_smartScrollRst = true; s_smartPrevBtns = 0; }

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

// ============================================================================
// HddSmart_Render
// ============================================================================

void HddSmart_Render(const DiagLogo& logo)
{
    // All declarations at top (C89 compliance)
    const HddData& d = s_data;
    const float LH2 = LINE_H - 1.f;
    const float startY = CONTENT_Y + 6.f;
    const float hdrH = (LH2 - 2.f) + 3.f;
    const float firstRowY = startY + hdrH;
    const float CX_ID = LM;
    const float CX_NAME = LM + 28.f;
    const float CX_CUR = LM + 230.f;
    const float CX_WST = LM + 264.f;
    const float CX_THR = LM + 298.f;
    const float CX_RAW = LM + 336.f;
    int rowCount = 0;
    int visible;
    int maxScroll;
    int drawn;
    int ri;
    bool canScrollUp;
    bool canScrollDown;
    const char* hint;
    float y;
    float trackTop;
    float trackH;
    float thumbH;
    float ratio;
    float thumbTop;

    g_pDevice->BeginScene();

    // ---- Build filtered attr list ---------------------------------------
    if (d.smartOK)
    {
        const BYTE* p = d.smartBuf + 2;
        int rj;
        for (ri = 0; ri < SMART_MAX_ATTRS; ++ri, p += SMART_ATTR_SIZE)
        {
            BYTE id = p[0];
            if (id == 0) continue;
            s_smartRows[rowCount].id = id;
            s_smartRows[rowCount].cur = p[3];
            s_smartRows[rowCount].wst = p[4];
            s_smartRows[rowCount].thr = p[5];
            for (rj = 0; rj < 6; ++rj)
                s_smartRows[rowCount].raw[rj] = p[6 + rj];
            ++rowCount;
        }
    }

    // ---- Scroll reset on entry ------------------------------------------
    if (s_smartScrollRst) { s_smartScroll = 0; s_smartScrollRst = false; }

    // ---- Layout ---------------------------------------------------------
    visible = Ftoi((BOT_BAR_Y - firstRowY) / LH2);
    maxScroll = rowCount - visible;
    if (maxScroll < 0) maxScroll = 0;
    if (s_smartScroll > maxScroll) s_smartScroll = maxScroll;
    if (s_smartScroll < 0)         s_smartScroll = 0;
    canScrollUp = (s_smartScroll > 0);
    canScrollDown = (s_smartScroll < maxScroll);

    // ---- Input: scroll --------------------------------------------------
    {
        WORD cur = GetButtons();
        if (EdgeDown(cur, s_smartPrevBtns, BTN_DPAD_UP) && s_smartScroll > 0)        --s_smartScroll;
        if (EdgeDown(cur, s_smartPrevBtns, BTN_DPAD_DOWN) && s_smartScroll < maxScroll) ++s_smartScroll;
        s_smartPrevBtns = cur;
    }

    // ---- Hints ----------------------------------------------------------
    if (!d.smartOK)
        hint = "[Left] Drive Info    [B] Back";
    else if (d.smartExportDone)
        hint = d.smartExportOK
        ? "[A] Saved OK  [Up/Dn] Scroll  [Left] Drive Info  [B] Back"
        : "[A] Save fail [Up/Dn] Scroll  [Left] Drive Info  [B] Back";
    else
        hint = "[A] Save smart.txt  [Up/Dn] Scroll  [Left] Drive Info  [B] Back";

    DrawPageChrome(logo, "HDD SMART", hint);

    y = startY;

    // ---- Error states ---------------------------------------------------
    if (!d.smartSupported)
    {
        DrawText(LM, y, "SMART not supported by this drive.", 1.3f, COL_GRAY);
        g_pDevice->EndScene(); g_pDevice->Present(NULL, NULL, NULL, NULL); return;
    }
    if (!d.smartOK)
    {
        DrawText(LM, y, "SMART READ DATA failed.", 1.3f, COL_RED);
        g_pDevice->EndScene(); g_pDevice->Present(NULL, NULL, NULL, NULL); return;
    }

    // ---- Column headers -------------------------------------------------
    DrawText(CX_ID, y, "ID", 1.1f, COL_GRAY);
    DrawText(CX_NAME, y, "ATTRIBUTE", 1.1f, COL_GRAY);
    DrawText(CX_CUR, y, "CUR", 1.1f, COL_GRAY);
    DrawText(CX_WST, y, "WST", 1.1f, COL_GRAY);
    DrawText(CX_THR, y, "THR", 1.1f, COL_GRAY);
    DrawText(CX_RAW, y, "RAW VALUE", 1.1f, COL_GRAY);
    y += LH2 - 2.f;
    HLine(y, LM, SW - LM, COL_BORDER);
    y += 3.f;

    // ---- Scroll up indicator --------------------------------------------
    if (canScrollUp)
        DrawTextR(SW - LM, startY, "^ more", 1.0f, COL_DIM);

    // ---- Rows -----------------------------------------------------------
    drawn = 0;
    for (ri = s_smartScroll; ri < rowCount && drawn < visible; ++ri, ++drawn)
    {
        char idStr[4];
        char curStr[4];
        char wstStr[4];
        char thrStr[4];
        char nameBuf[22];
        char rawStr[20];
        char rb[4];
        bool isCrit;
        bool rawNonZero;
        bool highlight;
        const char* name;
        int rj, rk;
        const SmartRow& r = s_smartRows[ri];

        IntToHex(r.id, 2, idStr, sizeof(idStr));
        IntToStr(r.cur, curStr, sizeof(curStr));
        IntToStr(r.wst, wstStr, sizeof(wstStr));
        IntToStr(r.thr, thrStr, sizeof(thrStr));

        isCrit = SmartAttrCritical(r.id);
        rawNonZero = false;
        for (rj = 0; rj < 6; ++rj) if (r.raw[rj]) { rawNonZero = true; break; }
        highlight = isCrit && rawNonZero;

        if (highlight)
            FillRect(LM - 2.f, y - 1.f, SW - LM + 2.f, y + LH2 - 2.f,
                D3DCOLOR_XRGB(48, 12, 12));

        name = SmartAttrName(r.id);
        if (name) StrCopy(nameBuf, sizeof(nameBuf), name);
        else { StrCopy(nameBuf, sizeof(nameBuf), "Attr 0x"); StrCat2(nameBuf, sizeof(nameBuf), nameBuf, idStr); }

        DrawText(CX_ID, y, idStr, 1.05f, COL_DIM);
        DrawText(CX_NAME, y, nameBuf, 1.1f, highlight ? COL_RED : COL_WHITE);
        DrawText(CX_CUR, y, curStr, 1.05f, COL_CYAN);
        DrawText(CX_WST, y, wstStr, 1.05f, COL_GRAY);
        DrawText(CX_THR, y, thrStr, 1.05f, COL_DIM);

        rawStr[0] = '\0';
        for (rk = 5; rk >= 0; --rk)
        {
            IntToHex(r.raw[rk], 2, rb, sizeof(rb));
            StrCat2(rawStr, sizeof(rawStr), rawStr, rb);
            if (rk > 0) StrCat2(rawStr, sizeof(rawStr), rawStr, " ");
        }
        DrawText(CX_RAW, y, rawStr, 1.0f, rawNonZero ? COL_YELLOW : COL_DIM);
        y += LH2;
    }

    // ---- Scroll down indicator ------------------------------------------
    if (canScrollDown)
    {
        int remaining = rowCount - (s_smartScroll + drawn);
        char remStr[16];
        char numBuf[8];
        StrCopy(remStr, sizeof(remStr), "v ");
        IntToStr(remaining, numBuf, sizeof(numBuf));
        StrCat2(remStr, sizeof(remStr), remStr, numBuf);
        StrCat2(remStr, sizeof(remStr), remStr, " more");
        DrawTextR(SW - LM, y, remStr, 1.0f, COL_DIM);
    }

    // ---- Scrollbar (right edge) -----------------------------------------
    if (rowCount > visible)
    {
        trackTop = firstRowY;
        trackH = (float)visible * LH2;
        thumbH = (visible < rowCount) ? trackH * visible / (float)rowCount : trackH;
        ratio = (rowCount > visible) ? (float)s_smartScroll / (float)(rowCount - visible) : 0.f;
        thumbTop = trackTop + (trackH - thumbH) * ratio;
        VLine(SW - LM + 8.f, trackTop, trackTop + trackH, COL_DIM);
        FillRect(SW - LM + 7.f, thumbTop, SW - LM + 10.f, thumbTop + thumbH, COL_GRAY);
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}