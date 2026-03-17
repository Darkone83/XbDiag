// lcd.cpp
// XbDiag - Physical LCD display driver
//
// Drives a US2066-compatible 20x4 character OLED at SMBus 0x78 (7-bit 0x3C).
// Protocol: [0x80][cmd] = HD44780 command, [0x40][data] = character data.
// Row DDRAM offsets: 0x00 / 0x20 / 0x40 / 0x60  (PrometheOS compatible).
//
// LCD_Begin()   — call at app startup. Detects display, runs init, shows splash.
// LCD_SetData() — call once after SysInfo has loaded its data.
// LCD_Tick()    — call every frame from main loop.
// LCD_OnExit()  — clears display on app exit.
//
// Key combos (all three buttons simultaneously, edge-triggered):
//   START + A + WHITE  — enable display
//   START + A + BLACK  — disable display

#include "lcd.h"
#include "DiagCommon.h"
#include "input.h"
#include "FtpServ.h"
#include <xtl.h>

extern "C" LONG __stdcall ExQueryNonVolatileSetting(
    ULONG ValueIndex, ULONG* Type, void* Value, ULONG ValueLength, ULONG* ResultLength);

// Forward declarations
static void LCDCmd(BYTE cmd);
static void LCDGoto(int row, int col);
static void LCDChar(BYTE data);
static void DrawCurrentPage();
static void ShadowInvalidate();

// ============================================================================
// Configuration
// ============================================================================

#define LCD_ADDR              0x78
#define LCD_COLS              20
#define LCD_ROWS              4
#define LCD_PAGE_COUNT        4      // pages 1-4 (no splash in rotation)
#define LCD_PAGE_INTERVAL_MS  5000
#define LCD_SENSOR_INTERVAL_MS 1000

#define HD44780_CLEAR     0x01
#define HD44780_HOME      0x02
#define HD44780_ENTRY_SET 0x06
#define HD44780_DISP_ON   0x0C
#define HD44780_FUNC_SET  0x38
#define HD44780_DDRAM(a)  (0x80 | (BYTE)(a))

static const BYTE k_rowBase[4] = { 0x00, 0x20, 0x40, 0x60 };

#define VAL_COL 9

#define COMBO_ENABLE  (BTN_START | BTN_A | BTN_WHITE)
#define COMBO_DISABLE (BTN_START | BTN_A | BTN_BLACK)

// ============================================================================
// State
// ============================================================================

static bool  s_present = false;
static bool  s_dataReady = false;
static bool  s_enabled = true;
static bool  s_ftpWasActive = false;
static int   s_page = 1;
static DWORD s_pageTimer = 0;
static DWORD s_sensorTimer = 0;

// Static data
static char s_boardRev[16] = "---";
static char s_modchip[24] = "---";
static char s_cpuMHz[16] = "---";
static char s_gpuMHz[16] = "---";
static char s_hddModel[48] = "---";
static char s_hddSize[12] = "---";
static char s_hddUDMA[8] = "---";
static char s_ipAddr[20] = "No Link";
static char s_macAddr[20] = "---";

// Live sensor data
static BYTE s_tempCPU = 0;
static BYTE s_tempBoard = 0;
static BYTE s_fanPct = 0;
static bool s_sensorOK = false;
static bool s_is16 = false;

// Shadow buffer — skip SMBus writes when cell unchanged
static BYTE s_shadow[LCD_ROWS][LCD_COLS];
static int  s_hwRow = -1;
static int  s_hwCol = -1;
static int  s_lRow = -1;
static int  s_lCol = -1;

// ============================================================================
// Shadow
// ============================================================================

static void ShadowInvalidate()
{
    for (int r = 0; r < LCD_ROWS; ++r)
        for (int c = 0; c < LCD_COLS; ++c)
            s_shadow[r][c] = 0x01;
    s_hwRow = -1; s_hwCol = -1;
    s_lRow = -1; s_lCol = -1;
}

// ============================================================================
// Low-level write helpers
// ============================================================================

static void LCDCmd(BYTE cmd)
{
    SMBusWrite(LCD_ADDR, 0x80, cmd);
    s_hwRow = -1; s_hwCol = -1;
    s_lRow = -1; s_lCol = -1;
}

static void LCDChar(BYTE data)
{
    if (data != 0xFF && (data < 0x20 || data > 0x7E)) data = ' ';

    if (s_lRow < 0 || s_lRow >= LCD_ROWS || s_lCol < 0 || s_lCol >= LCD_COLS)
    {
        SMBusWrite(LCD_ADDR, 0x40, data);
        return;
    }

    if (s_shadow[s_lRow][s_lCol] == data)
    {
        s_lCol++;
        if (s_lCol >= LCD_COLS) { s_lRow++; s_lCol = 0; }
        return;
    }

    if (s_hwRow != s_lRow || s_hwCol != s_lCol)
    {
        SMBusWrite(LCD_ADDR, 0x80, HD44780_DDRAM(k_rowBase[s_lRow] + (BYTE)s_lCol));
        s_hwRow = s_lRow;
        s_hwCol = s_lCol;
    }

    SMBusWrite(LCD_ADDR, 0x40, data);
    s_shadow[s_lRow][s_lCol] = data;
    s_lCol++; s_hwCol++;
    if (s_lCol >= LCD_COLS) { s_lRow++; s_lCol = 0; s_hwRow++; s_hwCol = 0; }
}

static void LCDGoto(int row, int col)
{
    if (row < 0 || row >= LCD_ROWS || col < 0 || col >= LCD_COLS) return;
    SMBusWrite(LCD_ADDR, 0x80, HD44780_DDRAM(k_rowBase[row] + (BYTE)col));
    s_hwRow = row; s_hwCol = col;
    s_lRow = row; s_lCol = col;
}

static void LCDPuts(const char* s, int width)
{
    int n = 0;
    while (s && *s && n < width)
    {
        BYTE c = (BYTE)*s++;
        if (c < 0x20 || c > 0x7E) c = ' ';
        LCDChar(c);
        ++n;
    }
    while (n < width) { LCDChar(' '); ++n; }
}

static void LCDHeader(const char* title)
{
    LCDGoto(0, 0);
    int len = 0; while (title[len]) ++len;
    int total = LCD_COLS - len - 2;
    int lDash = total / 2;
    int rDash = total - lDash;
    char buf[21]; int pos = 0;
    for (int i = 0; i < lDash && pos < LCD_COLS; ++i) buf[pos++] = '-';
    if (pos < LCD_COLS) buf[pos++] = ' ';
    for (int i = 0; i < len && pos < LCD_COLS; ++i) buf[pos++] = title[i];
    if (pos < LCD_COLS) buf[pos++] = ' ';
    for (int i = 0; i < rDash && pos < LCD_COLS; ++i) buf[pos++] = '-';
    buf[pos] = '\0';
    LCDPuts(buf, LCD_COLS);
}

static void LCDLabelVal(int row, const char* label, const char* val)
{
    LCDGoto(row, 0);
    int n = 0;
    while (label && *label && n < VAL_COL - 1) { LCDChar((BYTE)*label++); ++n; }
    while (n < VAL_COL) { LCDChar(' '); ++n; }
    LCDPuts(val, LCD_COLS - VAL_COL);
}

// ============================================================================
// Hardware init
// ============================================================================

static void LCDHardwareInit()
{
    Sleep(15);
    LCDCmd(HD44780_FUNC_SET); Sleep(5);
    LCDCmd(HD44780_FUNC_SET); Sleep(1);
    LCDCmd(HD44780_FUNC_SET);
    LCDCmd(HD44780_DISP_ON);
    LCDCmd(HD44780_CLEAR);    Sleep(2);
    LCDCmd(HD44780_ENTRY_SET);
}

// ============================================================================
// Sensors
// ============================================================================

static void LCDReadSensors()
{
    BYTE cpu = 0, board = 0, fanRaw = 0;
    if (s_is16)
    {
        bool ok = SMBusRead(SMBADDR_PIC, 0x09, cpu) &&
            SMBusRead(SMBADDR_PIC, 0x0A, board);
        s_sensorOK = ok;
        if (ok) { s_tempCPU = cpu; s_tempBoard = board; }
    }
    else
    {
        bool ok = SMBusRead(SMBADDR_ADM1032, 0x01, cpu) &&
            SMBusRead(SMBADDR_ADM1032, 0x00, board);
        s_sensorOK = ok;
        if (ok) { s_tempCPU = cpu; s_tempBoard = board; }
    }
    if (SMBusRead(SMBADDR_PIC, 0x10, fanRaw) && fanRaw <= 50)
        s_fanPct = (BYTE)((int)fanRaw * 2);
    else if (SMBusRead(SMBADDR_PIC, 0x06, fanRaw) && fanRaw <= 50)
        s_fanPct = (BYTE)((int)fanRaw * 2);
}

// ============================================================================
// Pages
// ============================================================================

static void DrawPageSplash()
{
    LCDGoto(0, 0); LCDPuts("** XbDiag v1.0.1 **", LCD_COLS);
    LCDGoto(1, 0); LCDPuts(" Team  Resurgent   ", LCD_COLS);
    LCDGoto(2, 0); LCDPuts("    Darkone83      ", LCD_COLS);
    LCDGoto(3, 0); LCDPuts("                   ", LCD_COLS);
}

static void DrawPageThermal()
{
    LCDHeader("THERMAL");
    char val[12];
    if (s_sensorOK) { IntToStr((int)s_tempCPU, val, sizeof(val)); StrCat2(val, sizeof(val), val, " C"); }
    else StrCopy(val, sizeof(val), "ERR");
    LCDLabelVal(1, "CPU:", val);

    if (s_sensorOK) { IntToStr((int)s_tempBoard, val, sizeof(val)); StrCat2(val, sizeof(val), val, " C"); }
    else StrCopy(val, sizeof(val), "ERR");
    LCDLabelVal(2, "Board:", val);

    IntToStr((int)s_fanPct, val, sizeof(val)); StrCat2(val, sizeof(val), val, " %");
    LCDLabelVal(3, "Fan:", val);
}

static void DrawPageClocks()
{
    LCDHeader("CLOCKS");
    LCDLabelVal(1, "CPU:", s_cpuMHz);
    LCDLabelVal(2, "GPU:", s_gpuMHz);
    char revLine[21];
    StrCopy(revLine, sizeof(revLine), s_boardRev);
    StrCat2(revLine, sizeof(revLine), revLine, " ");
    StrCat2(revLine, sizeof(revLine), revLine, (s_modchip[0] == 'N') ? "TSOP" : s_modchip);
    LCDLabelVal(3, "Rev:", revLine);
}

static void DrawPageStorage()
{
    LCDHeader("STORAGE");
    LCDGoto(1, 0); LCDPuts(s_hddModel, LCD_COLS);
    LCDLabelVal(2, "Size:", s_hddSize);
    LCDLabelVal(3, "Mode:", s_hddUDMA);
}

static void DrawPageNetwork()
{
    LCDHeader("NETWORK");
    LCDLabelVal(1, "IP:", s_ipAddr);
    LCDGoto(2, 0); LCDPuts("MAC:", LCD_COLS);
    LCDGoto(3, 0); LCDPuts(s_macAddr, LCD_COLS);
}

static void DrawPageFTP()
{
    const char* stateStr;
    switch (g_ftp.state)
    {
    case FTP_LISTEN:    stateStr = "LISTENING";  break;
    case FTP_CONNECTED: stateStr = "CONNECTED";  break;
    case FTP_TRANSFER:  stateStr = "TRANSFER";   break;
    default:            stateStr = "OFF";        break;
    }
    char line[21];
    StrCopy(line, sizeof(line), "FTP  ");
    StrCat2(line, sizeof(line), line, stateStr);
    LCDGoto(0, 0); LCDPuts(line, LCD_COLS);

    StrCopy(line, sizeof(line), s_ipAddr);
    StrCat2(line, sizeof(line), line, " :21");
    LCDGoto(1, 0); LCDPuts(line, LCD_COLS);

    bool xfer = (g_ftp.state == FTP_TRANSFER && g_ftp.xferType != XFER_NONE);
    if (xfer)
    {
        const char* verb = (g_ftp.xferType == XFER_RETR) ? "GET " :
            (g_ftp.xferType == XFER_STOR) ? "PUT " : "DIR ";
        StrCopy(line, sizeof(line), verb);
        StrCat2(line, sizeof(line), line, g_ftp.xferName);
        LCDGoto(2, 0); LCDPuts(line, LCD_COLS);

        int pct = 0;
        if (g_ftp.xferType == XFER_RETR && g_ftp.xferTotal > 0)
        {
            pct = (int)(((DWORD)100 * g_ftp.xferDone) / g_ftp.xferTotal);
            if (pct > 100) pct = 100;
        }
        else if (g_ftp.xferType == XFER_STOR)
            pct = (int)((GetTickCount() / 50) % 101);
        else pct = 50;

        int pos = 0;
        const int BAR_W = 12;
        int filled = (pct * BAR_W) / 100;
        line[pos++] = '[';
        for (int i = 0; i < BAR_W; ++i) line[pos++] = (i < filled) ? '#' : ' ';
        line[pos++] = ']'; line[pos++] = ' ';
        char pctBuf[8]; IntToStr(pct, pctBuf, sizeof(pctBuf));
        int pl = 0; while (pctBuf[pl]) ++pl;
        while (pl < 3) { line[pos++] = ' '; ++pl; }
        for (int i = 0; pctBuf[i]; ++i) line[pos++] = pctBuf[i];
        line[pos] = '\0';
        LCDGoto(3, 0); LCDPuts(line, LCD_COLS);
    }
    else
    {
        LCDGoto(2, 0); LCDPuts("                    ", LCD_COLS);
        LCDGoto(3, 0); LCDPuts("                    ", LCD_COLS);
    }
}

static void DrawCurrentPage()
{
    switch (s_page)
    {
    case 1: DrawPageThermal(); break;
    case 2: DrawPageClocks();  break;
    case 3: DrawPageStorage(); break;
    case 4: DrawPageNetwork(); break;
    default: s_page = 1; DrawPageThermal(); break;
    }
}

static void PageChange(int nextPage)
{
    SMBusWrite(LCD_ADDR, 0x80, HD44780_CLEAR);
    Sleep(2);
    ShadowInvalidate();
    s_page = nextPage;
    DrawCurrentPage();
}

// ============================================================================
// Public API
// ============================================================================

void LCD_Begin()
{
    s_present = false;
    s_dataReady = false;
    s_page = 1;

    BYTE dummy = 0;
    if (!SMBusRead(LCD_ADDR, 0x00, dummy))
        return;

    LCDHardwareInit();
    ShadowInvalidate();
    Sleep(10);

    s_pageTimer = GetTickCount();
    s_sensorTimer = GetTickCount();

    // Show splash until LCD_SetData fires
    DrawPageSplash();

    s_present = true;
}

void LCD_SetData(const LCDData& data)
{
    if (!s_present) return;

    const char* r = data.boardRev ? data.boardRev : "";
    s_is16 = (r[0] == '1' && r[1] == '.' && r[2] == '6');

    StrCopy(s_boardRev, sizeof(s_boardRev), data.boardRev ? data.boardRev : "?");
    StrCopy(s_modchip, sizeof(s_modchip), data.modchipName ? data.modchipName : "?");
    StrCopy(s_cpuMHz, sizeof(s_cpuMHz), data.cpuSpeedMHz ? data.cpuSpeedMHz : "?");
    StrCopy(s_gpuMHz, sizeof(s_gpuMHz), data.gpuSpeedMHz ? data.gpuSpeedMHz : "?");
    StrCopy(s_hddModel, sizeof(s_hddModel), data.hddModel ? data.hddModel : "?");
    StrCopy(s_hddSize, sizeof(s_hddSize), data.hddSizeGB ? data.hddSizeGB : "?");
    StrCopy(s_hddUDMA, sizeof(s_hddUDMA), data.hddUDMA ? data.hddUDMA : "?");
    StrCopy(s_ipAddr, sizeof(s_ipAddr), data.ipAddr ? data.ipAddr : "No Link");
    StrCopy(s_macAddr, sizeof(s_macAddr), data.macAddr ? data.macAddr : "?");

    s_dataReady = true;

    // Move off splash to thermal
    LCDReadSensors();
    PageChange(1);
    s_pageTimer = GetTickCount();
}

void LCD_Tick(WORD curButtons, WORD prevButtons)
{
    if (!s_present) return;

    DWORD now = GetTickCount();

    // ---- Key combos ----
    bool enableCombo = ((curButtons & COMBO_ENABLE) == COMBO_ENABLE) &&
        ((prevButtons & COMBO_ENABLE) != COMBO_ENABLE);
    bool disableCombo = ((curButtons & COMBO_DISABLE) == COMBO_DISABLE) &&
        ((prevButtons & COMBO_DISABLE) != COMBO_DISABLE);

    if (enableCombo && !s_enabled)
    {
        s_enabled = true;
        LCDHardwareInit();
        ShadowInvalidate();
        DrawCurrentPage();
        s_pageTimer = now;
        return;
    }
    if (disableCombo && s_enabled)
    {
        s_enabled = false;
        SMBusWrite(LCD_ADDR, 0x80, HD44780_CLEAR);
        Sleep(2);
        SMBusWrite(LCD_ADDR, 0x80, 0x08);  // display off
        return;
    }
    if (!s_enabled) return;

    // ---- FTP override ----
    bool ftpActive = (g_ftp.state != FTP_OFF);
    if (ftpActive)
    {
        if (!s_ftpWasActive)
        {
            SMBusWrite(LCD_ADDR, 0x80, HD44780_CLEAR);
            Sleep(2);
            ShadowInvalidate();
            s_ftpWasActive = true;
        }
        DrawPageFTP();
        return;
    }
    else if (s_ftpWasActive)
    {
        s_ftpWasActive = false;
        PageChange(s_page);
        s_pageTimer = now;
        return;
    }

    // ---- Hold splash until data arrives ----
    if (!s_dataReady) return;

    // ---- Auto-cycle ----
    if ((now - s_pageTimer) >= LCD_PAGE_INTERVAL_MS)
    {
        int nextPage = (s_page % LCD_PAGE_COUNT) + 1;
        PageChange(nextPage);
        s_pageTimer = now;
        return;
    }

    // ---- Live sensor refresh ----
    if ((now - s_sensorTimer) >= LCD_SENSOR_INTERVAL_MS)
    {
        s_sensorTimer = now;
        LCDReadSensors();
        if (s_page == 1)
            DrawPageThermal();
    }
}

void LCD_OnExit()
{
    if (!s_present) return;
    SMBusWrite(LCD_ADDR, 0x80, HD44780_CLEAR);
    Sleep(2);
    ShadowInvalidate();
    LCDGoto(1, 0); LCDPuts("   XbDiag exiting   ", LCD_COLS);
}

bool LCD_IsPresent()
{
    return s_present;
}