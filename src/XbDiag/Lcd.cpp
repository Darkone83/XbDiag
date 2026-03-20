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

// For CerBIOS E: partition mount in CerBiosOwnsLCD()
typedef struct { USHORT Length; USHORT MaximumLength; char* Buffer; } LCD_XBOX_STRING;
extern "C" LONG WINAPI IoCreateSymbolicLink(LCD_XBOX_STRING* symLink, LCD_XBOX_STRING* target);

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

// Simple null-safe string equality (no CRT strcmp, no StrCmp in DiagCommon)
static bool StrEq(const char* a, const char* b)
{
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == '\0' && *b == '\0';
}

// ============================================================================
// CerBIOS LCD conflict detection
// Parses E:\Cerbios\cerbios.ini inline — no external module.
// Returns true if CerBIOS owns the SMBus LCD at the given 7-bit address.
// ============================================================================

#define CERBIOS_INI_PATH "E:\\Cerbios\\cerbios.ini"

static bool CerBiosOwnsLCD(BYTE addr7bit)
{
    // E: = \Device\Harddisk0\Partition1.  If XbDiag launched from C: or a
    // modchip, E: may not be mounted yet.  Mount it now — 0xC0000035
    // (already exists) is fine, any other error means no HDD, return false.
    {
        char linkBuf[] = "\\??\\E:";
        char devBuf[] = "\\Device\\Harddisk0\\Partition1";
        LCD_XBOX_STRING sLink = { 6,  7,  linkBuf };
        LCD_XBOX_STRING sDev = { 28, 29, devBuf };
        LONG r = IoCreateSymbolicLink(&sLink, &sDev);
        // 0 = mounted fresh, 0xC0000035 = already mounted — both fine
        if (r != 0 && r != (LONG)0xC0000035)
            return false;
    }

    HANDLE hFile = CreateFileA(CERBIOS_INI_PATH, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;  // not a CerBIOS install

    bool lcdEnable = false;
    int  lcdBus = 0;
    BYTE lcdAddr = 0x3C;  // CerBIOS default
    bool gotEnable = false, gotBus = false, gotAddr = false;

    char line[256];
    DWORD nRead;
    int lineLen = 0;

    while (!(gotEnable && gotBus && gotAddr))
    {
        // Read one line
        lineLen = 0;
        bool eof = false;
        while (lineLen < (int)sizeof(line) - 1)
        {
            char ch;
            if (!ReadFile(hFile, &ch, 1, &nRead, NULL) || nRead == 0) { eof = true; break; }
            if (ch == '\n') break;
            if (ch == '\r') continue;
            line[lineLen++] = ch;
        }
        line[lineLen] = '\0';
        if (eof && lineLen == 0) break;

        // Skip blank lines and comments
        int s = 0;
        while (line[s] == ' ' || line[s] == '\t') ++s;
        if (line[s] == '\0' || line[s] == ';') continue;

        // Find '='
        int eq = -1;
        for (int i = s; line[i]; ++i) if (line[i] == '=') { eq = i; break; }
        if (eq < 0) continue;

        // Extract and lowercase key
        char key[32]; int kl = 0;
        for (int i = s; i < eq && kl < 31; ++i)
        {
            char c = line[i];
            if (c == ' ' || c == '\t') continue;
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            key[kl++] = c;
        }
        key[kl] = '\0';

        // Extract and lowercase value (strip inline comment)
        char val[32]; int vl = 0;
        int vs = eq + 1;
        while (line[vs] == ' ' || line[vs] == '\t') ++vs;
        for (int i = vs; line[i] && line[i] != ';' && vl < 31; ++i)
        {
            char c = line[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            val[vl++] = c;
        }
        // Trim trailing whitespace from val
        while (vl > 0 && (val[vl - 1] == ' ' || val[vl - 1] == '\t')) --vl;
        val[vl] = '\0';

        // Match keys
        if (!gotEnable && StrEq(key, "inapplcdenable"))
        {
            lcdEnable = (StrEq(val, "true"));
            gotEnable = true;
        }
        else if (!gotBus && StrEq(key, "lcdbus"))
        {
            lcdBus = (int)(val[0] - '0');
            gotBus = true;
        }
        else if (!gotAddr && StrEq(key, "lcdi2caddr"))
        {
            // Parse hex: "0x3c" or "0x3C"
            BYTE result = 0;
            const char* p = val;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
            while (*p)
            {
                char hc = *p++;
                int digit = 0;
                if (hc >= '0' && hc <= '9') digit = hc - '0';
                else if (hc >= 'a' && hc <= 'f') digit = 10 + (hc - 'a');
                else break;
                result = (BYTE)((result << 4) | (BYTE)digit);
            }
            lcdAddr = result;
            gotAddr = true;
        }
    }

    CloseHandle(hFile);

    // Only block if CerBIOS has InAppLCDEnable=True, on the SMBus (bus 0),
    // at the same 7-bit address we would use.
    return lcdEnable && (lcdBus == 0) && (lcdAddr == addr7bit);
}

// Strip any non-numeric suffix from a version string for compact display.
// "1.0.2 Beta" -> "1.0.2"   "1.0.2" -> "1.0.2"
static void StripVersionSuffix(const char* src, char* dst, int dstLen)
{
    int i = 0;
    while (src[i] && i < dstLen - 1)
    {
        char ch = src[i];
        if (ch != '.' && (ch < '0' || ch > '9')) break;
        dst[i] = ch;
        ++i;
    }
    dst[i] = '\0';
}

// Center a string within LCD_COLS characters.
// Fills out (must be >= LCD_COLS+1 bytes) with left-pad spaces, the text,
// then right-pad spaces to exactly LCD_COLS chars, null-terminated.
static void LCDCenter(char* out, const char* s)
{
    int len = 0; while (s[len]) ++len;
    if (len >= LCD_COLS)
    {
        for (int i = 0; i < LCD_COLS; ++i) out[i] = s[i];
        out[LCD_COLS] = '\0';
        return;
    }
    int total = LCD_COLS - len;
    int lPad = total / 2;
    int rPad = total - lPad;
    int pos = 0;
    for (int i = 0; i < lPad; ++i) out[pos++] = ' ';
    for (int i = 0; i < len; ++i) out[pos++] = s[i];
    for (int i = 0; i < rPad; ++i) out[pos++] = ' ';
    out[pos] = '\0';
}

static void DrawPageSplash()
{
    // Build "XbDiag v1.0.1" and center it within "** ... **" borders.
    // Inner field = LCD_COLS - 4 (two '*' each side).
    // Read version directly from D:\XbDiag.ver — Update_GetLocalVersion() may
    // not be populated yet since LCD_Begin() runs before Update_StartBootCheck().
    char verNum[16];
    verNum[0] = '\0';
    {
        HANDLE hv = CreateFileA("D:\\XbDiag.ver", GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hv != INVALID_HANDLE_VALUE)
        {
            char buf[32]; DWORD rd = 0;
            if (ReadFile(hv, buf, sizeof(buf) - 1, &rd, NULL) && rd > 0)
            {
                buf[rd] = '\0';
                // Strip whitespace
                int e = (int)rd - 1;
                while (e >= 0 && (buf[e] == '\r' || buf[e] == '\n' ||
                    buf[e] == ' ' || buf[e] == '\t')) buf[e--] = '\0';
                StripVersionSuffix(buf, verNum, sizeof(verNum));
            }
            CloseHandle(hv);
        }
        if (verNum[0] == '\0') StrCopy(verNum, sizeof(verNum), "?.?.?");
    }
    char title[17];
    StrCopy(title, sizeof(title), "XbDiag v");
    StrCat2(title, sizeof(title), title, verNum);

    char line0[21];
    int tlen = 0; while (title[tlen]) ++tlen;
    int inner = LCD_COLS - 4;
    if (tlen <= inner)
    {
        int total = inner - tlen;
        int lPad = total / 2;
        int rPad = total - lPad;
        int pos = 0;
        line0[pos++] = '*'; line0[pos++] = '*';
        for (int i = 0; i < lPad; ++i) line0[pos++] = ' ';
        for (int i = 0; i < tlen; ++i) line0[pos++] = title[i];
        for (int i = 0; i < rPad; ++i) line0[pos++] = ' ';
        line0[pos++] = '*'; line0[pos++] = '*';
        line0[pos] = '\0';
    }
    else
    {
        LCDCenter(line0, title);
    }

    char line1[21]; LCDCenter(line1, "Team Resurgent");
    char line2[21]; LCDCenter(line2, "Darkone83");

    LCDGoto(0, 0); LCDPuts(line0, LCD_COLS);
    LCDGoto(1, 0); LCDPuts(line1, LCD_COLS);
    LCDGoto(2, 0); LCDPuts(line2, LCD_COLS);
    LCDGoto(3, 0); LCDPuts("                    ", LCD_COLS);
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

    // Cerbios 3.x.x has a persistent background LCD task that continues
    // driving the display even when a third-party XBE is running.
    // If it owns the SMBus LCD at our address, any write from us will
    // corrupt the display or cause a bus collision.
    // CerBiosOwnsLCD() reads E:\Cerbios\cerbios.ini — if InAppLCDEnable=True
    // on bus 0 at our address, stay off the bus entirely.
    if (CerBiosOwnsLCD(0x3C))
        return;

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

    // ---- Live sensor refresh (runs even before SysInfo data is available) ----
    if ((now - s_sensorTimer) >= LCD_SENSOR_INTERVAL_MS)
    {
        s_sensorTimer = GetTickCount();  // recapture after potential SMBus delay
        LCDReadSensors();

        // If SysInfo data hasn't arrived yet, start the auto-cycle now from
        // thermal so the display doesn't stay stuck on splash indefinitely.
        if (!s_dataReady)
        {
            s_dataReady = true;
            PageChange(1);
            s_pageTimer = GetTickCount();
            return;
        }

        if (s_page == 1)
            DrawPageThermal();
    }

    // ---- Hold until data ready ----
    if (!s_dataReady) return;

    // ---- Auto-cycle ----
    if ((now - s_pageTimer) >= LCD_PAGE_INTERVAL_MS)
    {
        int nextPage = (s_page % LCD_PAGE_COUNT) + 1;
        PageChange(nextPage);
        s_pageTimer = now;
        return;
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