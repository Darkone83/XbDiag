// HddSmart.cpp
// XbDiag - HDD SMART view.
//
// Renders the SMART attribute table and exports D:\smart.txt.
// All data comes from s_data.smartBuf (populated by LoadData in HddInfo.cpp).
// Attribute names, raw format decode, and vendor overrides via HddSmartDB.hpp.

#include "HddSmart.h"
#include "HddInfo.h"
#include "HddSmartDB.hpp"
#include "font.h"
#include "input.h"
#include <xtl.h>

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

    const char* col = "\r\nID    Name                 Cur  Wst  Thr  Value\r\n"
        "----  -------------------  ---  ---  ---  --------------------\r\n";
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

        // Decoded raw value via DB
        char rawStr[24];
        const SmartAttrDef* def = SmartDB_Lookup(d.model, id);
        if (def)
            SmartDB_FormatRaw(def->fmt, raw, rawStr, sizeof(rawStr));
        else
        {
            // fallback hex
            char rb[4]; int j;
            rawStr[0] = '\0';
            for (j = 5; j >= 0; --j)
            {
                IntToHex(raw[j], 2, rb, sizeof(rb));
                StrCat2(rawStr, sizeof(rawStr), rawStr, rb);
                if (j > 0) StrCat2(rawStr, sizeof(rawStr), rawStr, " ");
            }
        }

        // Attribute name via DB
        char nameBuf[22];
        if (def && def->name)
            StrCopy(nameBuf, sizeof(nameBuf), def->name);
        else
        {
            StrCopy(nameBuf, sizeof(nameBuf), "Attr 0x");
            StrCat2(nameBuf, sizeof(nameBuf), nameBuf, idStr);
        }
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
static int      s_smartRowCount = 0;  // populated by HddSmart_Render, read by HTTP server

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
    s_smartRowCount = rowCount;

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
    DrawText(CX_RAW, y, "VALUE", 1.1f, COL_GRAY);
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
        char rawStr[24];
        bool isCrit;
        bool rawNonZero;
        bool highlight;
        const SmartAttrDef* def;
        int rj;
        const SmartRow& r = s_smartRows[ri];

        IntToHex(r.id, 2, idStr, sizeof(idStr));
        IntToStr(r.cur, curStr, sizeof(curStr));
        IntToStr(r.wst, wstStr, sizeof(wstStr));
        IntToStr(r.thr, thrStr, sizeof(thrStr));

        def = SmartDB_Lookup(d.model, r.id);

        isCrit = def ? def->critical : false;
        rawNonZero = false;
        for (rj = 0; rj < 6; ++rj) if (r.raw[rj]) { rawNonZero = true; break; }

        // Highlight only when the manufacturer's threshold is actually tripped:
        // normalized VALUE <= THRESHOLD means the drive itself considers this a fail.
        // thr == 0 means no threshold defined — skip to avoid false positives.
        // This prevents flagging red on attributes like C7/A8 where the raw count
        // is nonzero but the drive's own normalized value is still above threshold.
        highlight = isCrit && (r.thr > 0) && (r.cur <= r.thr);

        if (highlight)
            FillRect(LM - 2.f, y - 1.f, SW - LM + 2.f, y + LH2 - 2.f,
                D3DCOLOR_XRGB(48, 12, 12));

        if (def && def->name)
            StrCopy(nameBuf, sizeof(nameBuf), def->name);
        else
        {
            StrCopy(nameBuf, sizeof(nameBuf), "Attr 0x");
            StrCat2(nameBuf, sizeof(nameBuf), nameBuf, idStr);
        }

        // Decoded raw value — DB format, fallback to hex
        if (def)
            SmartDB_FormatRaw(def->fmt, r.raw, rawStr, sizeof(rawStr));
        else
        {
            char rb[4]; int rk;
            rawStr[0] = '\0';
            for (rk = 5; rk >= 0; --rk)
            {
                IntToHex(r.raw[rk], 2, rb, sizeof(rb));
                StrCat2(rawStr, sizeof(rawStr), rawStr, rb);
                if (rk > 0) StrCat2(rawStr, sizeof(rawStr), rawStr, " ");
            }
        }

        DrawText(CX_ID, y, idStr, 1.05f, COL_DIM);
        DrawText(CX_NAME, y, nameBuf, 1.1f, highlight ? COL_RED : COL_WHITE);
        DrawText(CX_CUR, y, curStr, 1.05f, COL_CYAN);
        DrawText(CX_WST, y, wstStr, 1.05f, COL_GRAY);
        DrawText(CX_THR, y, thrStr, 1.05f, COL_DIM);
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

// ============================================================================
// HTTP server accessors -- expose decoded SMART rows without duplicating the DB
// ============================================================================

int HddSmart_GetRowCount()
{
    return s_smartRowCount;
}

bool HddSmart_GetRow(int idx,
    char* idBuf, int idLen,
    char* nameBuf, int nameLen,
    char* valBuf, int valLen,
    bool* outCrit, bool* outTripped)
{
    const SmartAttrDef* def;
    const char* name;
    bool isCrit;
    bool tripped;

    if (idx < 0 || idx >= s_smartRowCount) return false;
    {
        const SmartRow& r = s_smartRows[idx];

        IntToHex((unsigned)r.id, 2, idBuf, idLen);

        def = SmartDB_Lookup(s_data.model, r.id);
        name = def ? def->name : NULL;
        if (name)
            StrCopy(nameBuf, nameLen, name);
        else
        {
            char hex[4];
            StrCopy(nameBuf, nameLen, "Attr 0x");
            IntToHex((unsigned)r.id, 2, hex, sizeof(hex));
            StrCat2(nameBuf, nameLen, nameBuf, hex);
        }

        if (def)
            SmartDB_FormatRaw(def->fmt, r.raw, valBuf, valLen);
        else
            SmartDB_FormatRaw(RF_HEX, r.raw, valBuf, valLen);

        isCrit = def ? def->critical : false;
        tripped = isCrit && r.thr > 0 && r.cur <= r.thr;
        *outCrit = isCrit;
        *outTripped = tripped;
    }
    return true;
}

bool HddSmart_GetRowRaw(int idx, int* cur, int* wst, int* thr)
{
    if (idx < 0 || idx >= s_smartRowCount) return false;
    *cur = (int)s_smartRows[idx].cur;
    *wst = (int)s_smartRows[idx].wst;
    *thr = (int)s_smartRows[idx].thr;
    return true;
}