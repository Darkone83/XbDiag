// SmBusScan.cpp
// XbDiag - SMBus Bus Scanner
//
// Layout (640x480 design space):
//
//   [TOP BAR 56px]
//   -------------------------------------------------------
//   Row  Grid 8x16 cells (CONTENT_Y+4 .. ~220)
//   lbls  0x00-0x7F hex addresses, color-coded by state
//         + vertical scan-progress bar on right edge
//   -------------------------------------------------------
//   [Cursor address info strip - 1 line]
//   [Legend - 1 line]
//   -------------------------------------------------------
//   [Info panel ~180px]
//     Normal:  known device card (name, description, register map)
//              or "no device / NAK" placeholder
//     [A] open: register read panel (live values)
//   -------------------------------------------------------
//   [BOT BAR 30px]
//
// Cell color states:
//   dim bg / dim text   = not yet scanned
//   dark bg / dark text = NAK  (no device)
//   teal bg / cyan text = ACK  unknown device
//   green bg / yel text = ACK  known Xbox device
//   white border ring   = cursor
//
// Controls:
//   D-pad  move cursor
//   [A]    read registers for selected ACK address  (toggle)
//   [B]    close register read / back to menu
//   [X]    re-scan all addresses
//
// To add a known device:
//   1. Add an entry to s_known[]
//   2. Add a RegDesc array  s_regs_XXXX[]
//   3. Add an entry to s_devRegs[]
//   That's it - everything else is automatic.

#include "SmBusScan.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

extern void RequestState(int newState);
static const int MSTATE_MENU = 0;

// ============================================================================
// Grid geometry  (design-space pixels, scaled by SX/SY at render time)
// ============================================================================

static const int   ADDR_COUNT = 128;
static const int   GRID_COLS = 16;
static const int   GRID_ROWS = 8;

static const float ROW_LBL_X = 8.f;     // "0x00" labels
static const float GRID_LM = 46.f;    // left edge of first cell
static const float GRID_TOP = CONTENT_Y + 6.f;
static const float CELL_W = 32.f;
static const float CELL_H = 18.f;
static const float CELL_GX = 2.f;
static const float CELL_GY = 2.f;
static const float GRID_W = GRID_COLS * (CELL_W + CELL_GX) - CELL_GX;   // 542
static const float GRID_H = GRID_ROWS * (CELL_H + CELL_GY) - CELL_GY;   // 158

// Progress bar sits just right of the grid
static const float PROG_X = GRID_LM + GRID_W + 5.f;
static const float PROG_W = 10.f;

// Divider between grid area and info area
static const float DIV_Y = GRID_TOP + GRID_H + 5.f;   // ~227

// Cursor info strip
static const float CINFO_Y = DIV_Y + 4.f;               // ~231
// Legend strip
static const float LEGEND_Y = CINFO_Y + LINE_H + 2.f;    // ~251

// Info panel
static const float PANEL_X = LM;
static const float PANEL_Y = LEGEND_Y + LINE_H + 6.f;   // ~275
static const float PANEL_W = SW - LM * 2.f;             // 576
static const float PANEL_BOTTOM = BOT_BAR_Y - 4.f;           // 446
static const float PANEL_H = PANEL_BOTTOM - PANEL_Y;    // ~171

// ============================================================================
// Known device table
// Add entries here to register new devices.
// ============================================================================

struct KnownDevice
{
    BYTE        addr;
    const char* name;       // short name shown in grid/cursor strip
    const char* desc;       // one-line description shown in info panel
    const char* notes;      // extra detail / register map summary
};

// 7-bit hardware addresses (sw-shifted = addr<<1 used in HAL calls)
// 0x10 = PIC16L/SMC  (sw 0x20)
// 0x45 = Conexant CX25871 video encoder  (sw 0x8A)
// 0x54 = EEPROM  (sw 0xA8)
// 0x4C = ADM1032  (sw 0x98)
// 0x68 = X-RTC DS1307  (sw 0xD0)
// 0x69 = ICS clock  (sw 0xD2)
// 0x6A = Focus FS454 video encoder  (sw 0xD4)
// 0x70 = Xcalibur video encoder  (sw 0xE0)
static const KnownDevice s_known[] =
{
    {
        0x10, "PIC16L",
        "PIC16L microcontroller  -  power, AV pack, eject, fan, LED, temps",
        "Reg 0x01=ver  0x04=AV pack  0x09=CPU temp  0x0A=MB temp  0x10=fan"
    },
    {
        0x45, "VID ENC",
        "Conexant CX25871 video encoder  (1.0, 1.1, 1.2, 1.3)",
        "Reg 0x00=chip ID  0x01=power  0x11=mode  0x30=output mode"
    },
    {
        0x54, "EEPROM",
        "93LC56 EEPROM  -  serial, region, MAC address, HDKEY",
        "Reg 0x00-0x11=confounder  0x34=serial  0xEA-0xEF=MAC"
    },
    {
        0x4C, "ADM1032",
        "ADM1032 thermal sensor  -  board ambient + CPU die (1.0-1.5 only)",
        "Reg 0x00=ambient(local)  0x01=CPU die(remote)  0x10=frac  0x04=status"
    },
    {
        0x69, "ICS CLK",
        "ICS clock generator  -  system bus / PCI clock synthesis",
        "Reg 0x00-0x05=clock config bytes (read-only on most boards)"
    },
    {
        0x6A, "FOCUS",
        "Focus FS454 video encoder  (1.4, 1.5)",
        "Reg 0x00=chip ID  0x01=power  0x6E=misc control"
    },
    {
        0x70, "XCALIBUR",
        "Xcalibur integrated video encoder  (1.6, 1.6b)",
        "Reg 0x00=chip ID  0x6E=misc control  (Conexant-derived)"
    },
    {
        0x68, "X-RTC",
        "DS1307 real-time clock  -  X-RTC expansion mod (optional)",
        "Reg 0x00=sec  0x01=min  0x02=hr  0x04=day  0x05=mon  0x06=yr  0x07=ctrl"
    },
    {
        0x27, "I2C DISP",
        "PCF8574 I2C backpack  -  common LCD/OLED display interface",
        "GPIO expander driving HD44780-compatible display via 4-bit bus"
    },
    {
        0x3C, "OLED",
        "SSD1306/SSD1309 OLED display  -  128x64 or 128x32 (addr 0x3C)",
        "Reg 0x00=cmd  0x40=data  SA0 pin low selects this address"
    },
    {
        0x3D, "OLED",
        "SSD1306/SSD1309 OLED display  -  128x64 or 128x32 (addr 0x3D)",
        "Reg 0x00=cmd  0x40=data  SA0 pin high selects this address"
    },
    {
        0x44, "HD MOD",
        "HDMI adapter (Chimeric/compatible)  -  7-bit 0x44, sw-addr 0x88",
        "Reg 0x57=ver_major  0x58=ver_minor  0x59=ver_patch"
    },
};
static const int s_knownCount = sizeof(s_known) / sizeof(s_known[0]);

// ============================================================================
// Register read descriptors per device
// To add registers for a new device, add a RegDesc array and an entry in
// s_devRegs[].  Max MAX_DETAIL_ROWS entries per device.
// ============================================================================

struct RegDesc { BYTE reg; const char* label; };

static const RegDesc s_regs_enc[] =
{
    { 0x00, "CHIP ID " },
    { 0x01, "POWER   " },
    { 0x11, "MODE    " },
    { 0x30, "OUT MODE" },
};
static const RegDesc s_regs_pic[] =
{
    { 0x01, "VERSION " },
    { 0x03, "SCRATCH " },
    { 0x04, "AV PACK " },
};
static const RegDesc s_regs_eeprom[] =
{
    { 0x00, "B[0x00] " },
    { 0x34, "SERIAL0 " },
    { 0xEA, "MAC[0]  " },
    { 0xEB, "MAC[1]  " },
    { 0xEC, "MAC[2]  " },
};
static const RegDesc s_regs_adm[] =
{
    { 0x00, "AMBIENT " },
    { 0x01, "CPU DIE " },
    { 0x04, "STATUS  " },
    { 0x20, "LIM HI  " },
};
static const RegDesc s_regs_ics[] =
{
    { 0x00, "BYTE 0  " },
    { 0x01, "BYTE 1  " },
    { 0x02, "BYTE 2  " },
};
static const RegDesc s_regs_smc[] =
{
    { 0x01, "VERSION " },
    { 0x02, "RST RSN " },
    { 0x03, "INT ST  " },
};
static const RegDesc s_regs_rtc[] =
{
    { 0x00, "SECONDS " },
    { 0x01, "MINUTES " },
    { 0x02, "HOURS   " },
    { 0x07, "CTRL    " },
};
static const RegDesc s_regs_oled[] =
{
    { 0x00, "CMD REG " },
};
static const RegDesc s_regs_hdmod[] =
{
    { 0x57, "VER MAJ " },
    { 0x58, "VER MIN " },
    { 0x59, "VER PTCH" },
};
static const RegDesc s_regs_generic[] =
{
    { 0x00, "REG 0x00" },
    { 0x01, "REG 0x01" },
    { 0x02, "REG 0x02" },
};

struct DeviceRegs { BYTE addr; const RegDesc* regs; int count; };

static const DeviceRegs s_devRegs[] =
{
    { 0x10, s_regs_pic,    3 },
    { 0x45, s_regs_enc,    4 },
    { 0x54, s_regs_eeprom, 5 },
    { 0x4C, s_regs_adm,    4 },
    { 0x69, s_regs_ics,    3 },
    { 0x6A, s_regs_enc,    4 },   // Focus - same reg layout as Conexant for basics
    { 0x70, s_regs_enc,    4 },   // Xcalibur
    { 0x68, s_regs_rtc,    4 },   // X-RTC DS1307
    { 0x3C, s_regs_oled,   1 },   // OLED 0x3C
    { 0x3D, s_regs_oled,   1 },   // OLED 0x3D
    { 0x44, s_regs_hdmod,  3 },   // HDMI adapter (Chimeric/compatible)
};
static const int s_devRegsCount = sizeof(s_devRegs) / sizeof(s_devRegs[0]);

// ============================================================================
// Runtime device table  — loaded from D:\smbid.id at startup (local XBE folder)
// Supplements the built-in s_known[].  Max 32 user entries.
// ============================================================================

static const int   USER_KNOWN_MAX = 32;
static const char* SMBID_PATH = "D:\\smbid.id";

struct UserDevice
{
    BYTE addr;
    char name[12];
    char desc[80];
    char notes[80];
};

static UserDevice s_userKnown[USER_KNOWN_MAX];
static int        s_userKnownCount = 0;
static bool       s_idFileLoaded = false;   // true = loaded from file (even if 0 entries)

// ---- Tiny string helpers (no CRT) ------------------------------------------

static bool IdIsHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int IdHexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

// Copy src -> dst (up to dstMax-1 chars), trim leading/trailing spaces, null-terminate
static void IdTrimCopy(char* dst, int dstMax, const char* src, int srcLen)
{
    // trim leading
    while (srcLen > 0 && (*src == ' ' || *src == '\t')) { ++src; --srcLen; }
    // trim trailing
    while (srcLen > 0 && (src[srcLen - 1] == ' ' || src[srcLen - 1] == '\t' ||
        src[srcLen - 1] == '\r' || src[srcLen - 1] == '\n'))
        --srcLen;
    if (srcLen >= dstMax) srcLen = dstMax - 1;
    for (int i = 0; i < srcLen; ++i) dst[i] = src[i];
    dst[srcLen] = '\0';
}

// ---- Parse one line: "0xAD | NAME | desc | notes"  -------------------------
// Returns true if the line produced a valid entry.
static bool IdParseLine(const char* line, int len, UserDevice& out)
{
    // Skip blank lines and comment lines (# or ;)
    int i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= len || line[i] == '#' || line[i] == ';' || line[i] == '\r' || line[i] == '\n')
        return false;

    // Find four pipe-delimited fields
    const char* fields[4] = { NULL, NULL, NULL, NULL };
    int          flens[4] = { 0, 0, 0, 0 };
    int field = 0;
    fields[0] = line;
    for (int j = 0; j < len && field < 3; ++j)
    {
        if (line[j] == '|')
        {
            flens[field] = (int)(line + j - fields[field]);
            ++field;
            fields[field] = line + j + 1;
        }
    }
    flens[field] = (int)(line + len - fields[field]);
    if (field < 3) return false;   // need at least 4 fields

    // Field 0: address — accept 0xNN or NN (hex)
    const char* af = fields[0];
    int alen = flens[0];
    while (alen > 0 && (*af == ' ' || *af == '\t')) { ++af; --alen; }
    while (alen > 0 && (af[alen - 1] == ' ' || af[alen - 1] == '\t')) --alen;
    if (alen >= 2 && af[0] == '0' && (af[1] == 'x' || af[1] == 'X')) { af += 2; alen -= 2; }
    if (alen < 1 || alen > 2) return false;
    if (!IdIsHexDigit(af[0])) return false;
    int addr = IdHexVal(af[0]);
    if (alen == 2) { if (!IdIsHexDigit(af[1])) return false; addr = addr * 16 + IdHexVal(af[1]); }
    if (addr > 0x7F) return false;

    out.addr = (BYTE)addr;
    IdTrimCopy(out.name, sizeof(out.name), fields[1], flens[1]);
    IdTrimCopy(out.desc, sizeof(out.desc), fields[2], flens[2]);
    IdTrimCopy(out.notes, sizeof(out.notes), fields[3], flens[3]);
    if (out.name[0] == '\0') return false;
    return true;
}

// ---- Write the default smbid.id to disk ------------------------------------
static bool IdWriteDefault()
{
    HANDLE hf = CreateFile(SMBID_PATH,
        GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;

    // File content — plain ASCII, written as one block
    static const char body[] =
        "# smbid.id  -  XbDiag SMBus device ID database\r\n"
        "# Edit this file to add your own known devices.\r\n"
        "#\r\n"
        "# Format (one device per line):\r\n"
        "#   address (hex) | short name (<=11 chars) | description | register map notes\r\n"
        "#\r\n"
        "# Lines beginning with # or ; are comments and are ignored.\r\n"
        "# Addresses are 7-bit (e.g. 0x10 for the PIC/SMC).\r\n"
        "# Entries in this file supplement the built-in device table.\r\n"
        "# If an address is in both, the built-in entry takes priority.\r\n"
        "#\r\n"
        "# Example (remove the leading # to activate):\r\n"
        "; 0x48 | LM75 TEMP | LM75 / LM75A temperature sensor | Reg 0x00=temp 0x01=config 0x02=hyst 0x03=limit\r\n"
        "#\r\n"
        "# Add your entries below this line:\r\n"
        "\r\n";

    DWORD written = 0;
    BOOL  ok = WriteFile(hf, body, (DWORD)(sizeof(body) - 1), &written, NULL);
    CloseHandle(hf);
    return ok && (written == sizeof(body) - 1);
}

// ---- Load smbid.id into s_userKnown[] --------------------------------------
static void IdLoad()
{
    s_userKnownCount = 0;
    s_idFileLoaded = false;

    HANDLE hf = CreateFile(SMBID_PATH,
        GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hf == INVALID_HANDLE_VALUE)
    {
        // File not present — generate it; fall back to internal DB either way
        IdWriteDefault();
        return;
    }

    // Read entire file (cap at 8KB — plenty for any reasonable ID list)
    static char buf[8192];
    DWORD bytesRead = 0;
    BOOL  ok = ReadFile(hf, buf, sizeof(buf) - 1, &bytesRead, NULL);
    CloseHandle(hf);
    if (!ok || bytesRead == 0) { s_idFileLoaded = true; return; }
    buf[bytesRead] = '\0';

    // Walk lines
    char* p = buf;
    char* end = buf + bytesRead;
    while (p < end && s_userKnownCount < USER_KNOWN_MAX)
    {
        // Find end of line
        char* nl = p;
        while (nl < end && *nl != '\n') ++nl;
        int lineLen = (int)(nl - p);

        UserDevice dev;
        if (IdParseLine(p, lineLen, dev))
            s_userKnown[s_userKnownCount++] = dev;

        p = (nl < end) ? nl + 1 : end;
    }

    s_idFileLoaded = true;
}

// ============================================================================
// Module state\n// ============================================================================

enum ScanPhase { PHASE_SCANNING, PHASE_DONE };

struct AddrResult { bool scanned; bool ack; };

static AddrResult s_results[ADDR_COUNT];
static ScanPhase  s_phase;
static int        s_scanNext;
static int        s_cursor;
static bool       s_regReadOpen;    // register read panel active
static WORD       s_prevBtns;

static const int  MAX_DETAIL_ROWS = 8;
struct DetailRow { char label[12]; char valStr[8]; bool ok; };
static DetailRow  s_detail[MAX_DETAIL_ROWS];
static int        s_detailCount;

// ============================================================================
// Helpers
// ============================================================================

static bool EdgeDown(WORD cur, WORD prev, WORD btn)
{
    return ((cur & btn) != 0) && ((prev & btn) == 0);
}

// Scratch KnownDevice used to surface a UserDevice through FindKnown's return type.
// Single static instance — only valid until the next call.
static KnownDevice s_userKnownScratch;

static const KnownDevice* FindKnown(int addr)
{
    // Built-in table has priority
    for (int i = 0; i < s_knownCount; ++i)
        if (s_known[i].addr == (BYTE)addr) return &s_known[i];
    // User-loaded entries from smbid.id
    for (int i = 0; i < s_userKnownCount; ++i)
    {
        if (s_userKnown[i].addr == (BYTE)addr)
        {
            s_userKnownScratch.addr = s_userKnown[i].addr;
            s_userKnownScratch.name = s_userKnown[i].name;
            s_userKnownScratch.desc = s_userKnown[i].desc;
            s_userKnownScratch.notes = s_userKnown[i].notes;
            return &s_userKnownScratch;
        }
    }
    return NULL;
}

static const DeviceRegs* FindDevRegs(int addr)
{
    for (int i = 0; i < s_devRegsCount; ++i)
        if (s_devRegs[i].addr == (BYTE)addr) return &s_devRegs[i];
    return NULL;
}

static void ResetScan()
{
    for (int i = 0; i < ADDR_COUNT; ++i)
    {
        s_results[i].scanned = false;
        s_results[i].ack = false;
    }
    s_scanNext = 0;
    s_phase = PHASE_SCANNING;
    s_regReadOpen = false;
}

static void OpenRegRead(int addr)
{
    s_regReadOpen = true;
    s_detailCount = 0;

    const DeviceRegs* dr = FindDevRegs(addr);
    const RegDesc* rp = dr ? dr->regs : s_regs_generic;
    int               rc = dr ? dr->count : 3;
    if (rc > MAX_DETAIL_ROWS) rc = MAX_DETAIL_ROWS;

    for (int i = 0; i < rc; ++i)
    {
        StrCopy(s_detail[i].label, sizeof(s_detail[i].label), rp[i].label);
        BYTE val = 0;
        s_detail[i].ok = SMBusRead((BYTE)(addr << 1), rp[i].reg, val);
        if (s_detail[i].ok)
        {
            char t[4]; IntToHex(val, 2, t, sizeof(t));
            StrCopy(s_detail[i].valStr, sizeof(s_detail[i].valStr), "0x");
            StrCat2(s_detail[i].valStr, sizeof(s_detail[i].valStr),
                s_detail[i].valStr, t);
        }
        else StrCopy(s_detail[i].valStr, sizeof(s_detail[i].valStr), "NAK");
    }
    s_detailCount = rc;
}

// ============================================================================
// OnEnter
// ============================================================================

void SmBusScan_OnEnter()
{
    s_prevBtns = 0;
    s_cursor = 0x10;   // start on PIC16L (7-bit 0x10 = sw 0x20)
    IdLoad();          // load / generate smbid.id; falls back to internal DB silently
    ResetScan();
}

// ============================================================================
// Draw helpers
// ============================================================================

static void DrawGrid()
{
    for (int row = 0; row < GRID_ROWS; ++row)
    {
        // Row label  "0x00" .. "0x70"
        float ly = GRID_TOP + row * (CELL_H + CELL_GY) + (CELL_H - LINE_H) * 0.5f;
        char lbl[6];
        StrCopy(lbl, sizeof(lbl), "0x");
        char t[4]; IntToHex((BYTE)(row * GRID_COLS), 2, t, sizeof(t));
        StrCat2(lbl, sizeof(lbl), lbl, t);
        DrawText(ROW_LBL_X, ly, lbl, 1.0f, COL_DIM);

        for (int col = 0; col < GRID_COLS; ++col)
        {
            int   addr = row * GRID_COLS + col;
            float cx = GRID_LM + col * (CELL_W + CELL_GX);
            float cy = GRID_TOP + row * (CELL_H + CELL_GY);
            bool  isCursor = (addr == s_cursor);

            // Cursor highlight ring (draw before cell so cell sits on top)
            if (isCursor)
                FillRect(cx - 1.f, cy - 1.f,
                    cx + CELL_W + 1.f, cy + CELL_H + 1.f,
                    COL_WHITE);

            // Cell bg + text color
            DWORD bg, fg;
            if (!s_results[addr].scanned)
            {
                bg = D3DCOLOR_XRGB(16, 20, 44);
                fg = D3DCOLOR_XRGB(55, 60, 88);
            }
            else if (!s_results[addr].ack)
            {
                bg = D3DCOLOR_XRGB(11, 13, 28);
                fg = D3DCOLOR_XRGB(36, 40, 58);
            }
            else
            {
                const KnownDevice* kd = FindKnown(addr);
                if (kd) { bg = D3DCOLOR_XRGB(26, 50, 16); fg = COL_YELLOW; }
                else { bg = D3DCOLOR_XRGB(14, 46, 60); fg = COL_CYAN; }
            }

            FillRect(cx, cy, cx + CELL_W, cy + CELL_H, bg);

            char hex[4]; IntToHex((BYTE)addr, 2, hex, sizeof(hex));
            float tw = TW(hex, 1.1f);
            DrawText(cx + (CELL_W - tw) * 0.5f,
                cy + (CELL_H - LINE_H) * 0.5f + 1.f,
                hex, 1.1f, isCursor ? COL_BG : fg);

            DWORD bc = isCursor ? COL_WHITE : COL_BORDER;
            HLine(cy, cx, cx + CELL_W, bc);
            HLine(cy + CELL_H, cx, cx + CELL_W, bc);
            VLine(cx, cy, cy + CELL_H, bc);
            VLine(cx + CELL_W, cy, cy + CELL_H, bc);
        }
    }
}

static void DrawProgressBar()
{
    float by = GRID_TOP;
    float bh = GRID_H;
    float fill = (s_phase == PHASE_DONE) ? 1.f
        : (s_scanNext > 0 ? (float)s_scanNext / (float)ADDR_COUNT : 0.f);

    FillRect(PROG_X, by, PROG_X + PROG_W, by + bh, D3DCOLOR_XRGB(14, 17, 36));
    if (fill > 0.f)
    {
        DWORD ca = (s_phase == PHASE_DONE) ? D3DCOLOR_XRGB(40, 200, 80) : COL_CYAN;
        DWORD cb = (s_phase == PHASE_DONE) ? D3DCOLOR_XRGB(20, 110, 40) : D3DCOLOR_XRGB(20, 90, 150);
        FillRectGrad(PROG_X, by + bh * (1.f - fill),
            PROG_X + PROG_W, by + bh, ca, cb);
    }
    HLine(by, PROG_X, PROG_X + PROG_W, COL_BORDER);
    HLine(by + bh, PROG_X, PROG_X + PROG_W, COL_BORDER);
    VLine(PROG_X, by, by + bh, COL_BORDER);
    VLine(PROG_X + PROG_W, by, by + bh, COL_BORDER);
}

static void DrawCursorStrip()
{
    // Horizontal divider
    HLine(DIV_Y, LM, SW - LM, COL_BORDER);

    char  info[64];
    char  addrHex[4]; IntToHex((BYTE)s_cursor, 2, addrHex, sizeof(addrHex));
    StrCopy(info, sizeof(info), "0x");
    StrCat2(info, sizeof(info), info, addrHex);
    StrCat2(info, sizeof(info), info, "  ");

    DWORD col;
    const KnownDevice* kd = FindKnown(s_cursor);
    if (!s_results[s_cursor].scanned)
    {
        StrCat2(info, sizeof(info), info, "pending scan");
        col = COL_DIM;
    }
    else if (!s_results[s_cursor].ack)
    {
        StrCat2(info, sizeof(info), info, "NAK  -  no device");
        col = D3DCOLOR_XRGB(55, 60, 85);
    }
    else if (kd)
    {
        StrCat2(info, sizeof(info), info, kd->name);
        col = COL_YELLOW;
    }
    else
    {
        StrCat2(info, sizeof(info), info, "ACK  -  unknown device");
        col = COL_CYAN;
    }
    DrawText(LM, CINFO_Y, info, 1.2f, col);

    // Right-aligned scan status
    const char* status = (s_phase == PHASE_SCANNING) ? "SCANNING..." : "SCAN COMPLETE";
    DWORD sc = (s_phase == PHASE_SCANNING) ? COL_CYAN : COL_GREEN;
    DrawTextR(SW - LM, CINFO_Y, status, 1.1f, sc);
}

static void DrawLegend()
{
    struct Swatch { DWORD bg; DWORD tc; const char* lbl; };
    static const Swatch sw[] =
    {
        { D3DCOLOR_XRGB(26,50,16),  COL_YELLOW,              "KNOWN"   },
        { D3DCOLOR_XRGB(14,46,60),  COL_CYAN,                "UNKNOWN" },
        { D3DCOLOR_XRGB(11,13,28),  D3DCOLOR_XRGB(36,40,58), "NAK"     },
        { D3DCOLOR_XRGB(16,20,44),  D3DCOLOR_XRGB(55,60,88), "PENDING" },
    };

    float sx = LM;
    float sh = LINE_H - 5.f;
    float sy = LEGEND_Y + 2.f;

    for (int i = 0; i < 4; ++i)
    {
        FillRect(sx, sy, sx + 10.f, sy + sh, sw[i].bg);
        HLine(sy, sx, sx + 10.f, COL_BORDER);
        HLine(sy + sh, sx, sx + 10.f, COL_BORDER);
        VLine(sx, sy, sy + sh, COL_BORDER);
        VLine(sx + 10.f, sy, sy + sh, COL_BORDER);
        DrawText(sx + 13.f, LEGEND_Y, sw[i].lbl, 1.05f, sw[i].tc);
        sx += 13.f + TW(sw[i].lbl, 1.05f) + 16.f;
    }
}

// Info panel - shows device card when cursor is on a known device,
// or the live register read when [A] is active.
static void DrawInfoPanel()
{
    // Panel background
    FillRectGrad(PANEL_X, PANEL_Y,
        PANEL_X + PANEL_W, PANEL_BOTTOM,
        D3DCOLOR_XRGB(15, 20, 48),
        D3DCOLOR_XRGB(10, 13, 32));
    HLine(PANEL_Y, PANEL_X, PANEL_X + PANEL_W, COL_CYAN);
    HLine(PANEL_BOTTOM, PANEL_X, PANEL_X + PANEL_W, COL_BORDER);
    VLine(PANEL_X, PANEL_Y, PANEL_BOTTOM, COL_BORDER);
    VLine(PANEL_X + PANEL_W, PANEL_Y, PANEL_BOTTOM, COL_BORDER);

    float px = PANEL_X + 10.f;
    float py = PANEL_Y + 6.f;

    const KnownDevice* kd = FindKnown(s_cursor);

    if (s_regReadOpen)
    {
        // ---- Live register read ----
        char title[20];
        StrCopy(title, sizeof(title), "REGISTERS  0x");
        char t[4]; IntToHex((BYTE)s_cursor, 2, t, sizeof(t));
        StrCat2(title, sizeof(title), title, t);
        DrawText(px, py, title, 1.25f, COL_YELLOW);
        py += LINE_H + 3.f;
        HLine(py, PANEL_X + 8.f, PANEL_X + PANEL_W - 8.f, COL_BORDER);
        py += 5.f;

        float c2x = PANEL_X + PANEL_W * 0.5f + 4.f;
        int   half = (s_detailCount + 1) / 2;
        for (int i = 0; i < s_detailCount; ++i)
        {
            float rx = (i < half) ? px : c2x;
            float ry = py + ((i < half) ? i : (i - half)) * LINE_H;
            DrawText(rx, ry, s_detail[i].label, 1.15f, COL_GRAY);
            DrawText(rx + 74.f, ry, s_detail[i].valStr, 1.2f,
                s_detail[i].ok ? COL_WHITE : COL_RED);
        }
    }
    else if (kd)
    {
        // ---- Known device info card ----
        char addrHex[4]; IntToHex((BYTE)s_cursor, 2, addrHex, sizeof(addrHex));

        // Title: "0x4C  ADM1032"
        char title[32];
        StrCopy(title, sizeof(title), "0x");
        StrCat2(title, sizeof(title), title, addrHex);
        StrCat2(title, sizeof(title), title, "  ");
        StrCat2(title, sizeof(title), title, kd->name);
        DrawText(px, py, title, 1.3f, COL_YELLOW);

        // ACK/NAK badge right-aligned
        if (s_results[s_cursor].scanned)
        {
            const char* badge = s_results[s_cursor].ack ? "ACK" : "NAK";
            DWORD bc = s_results[s_cursor].ack ? COL_GREEN : COL_RED;
            DrawTextR(PANEL_X + PANEL_W - 10.f, py, badge, 1.2f, bc);
        }
        py += LINE_H + 2.f;

        // Description
        DrawText(px, py, kd->desc, 1.15f, COL_WHITE);
        py += LINE_H + 3.f;
        HLine(py, PANEL_X + 8.f, PANEL_X + PANEL_W - 8.f, COL_BORDER);
        py += 5.f;

        // Register map notes
        DrawText(px, py, kd->notes, 1.1f, COL_GRAY);
        py += LINE_H + 4.f;

        // [A] prompt - only if device has ACKed
        if (s_results[s_cursor].scanned && s_results[s_cursor].ack)
            DrawText(px, py, "[A] Read registers", 1.1f, COL_CYAN);
    }
    else if (s_results[s_cursor].scanned && s_results[s_cursor].ack)
    {
        // ---- Unknown ACK device ----
        char addrHex[4]; IntToHex((BYTE)s_cursor, 2, addrHex, sizeof(addrHex));
        char title[32];
        StrCopy(title, sizeof(title), "0x");
        StrCat2(title, sizeof(title), title, addrHex);
        StrCat2(title, sizeof(title), title, "  UNKNOWN DEVICE");
        DrawText(px, py, title, 1.3f, COL_CYAN);
        DrawTextR(PANEL_X + PANEL_W - 10.f, py, "ACK", 1.2f, COL_GREEN);
        py += LINE_H + 4.f;
        DrawText(px, py, "Device responded to address probe.", 1.15f, COL_GRAY);
        py += LINE_H;
        DrawText(px, py, "Not in known device table.", 1.15f, COL_GRAY);
        py += LINE_H + 4.f;
        DrawText(px, py, "[A] Probe registers 0x00-0x02", 1.1f, COL_CYAN);
    }
    else
    {
        // ---- No device / not yet scanned ----
        char addrHex[4]; IntToHex((BYTE)s_cursor, 2, addrHex, sizeof(addrHex));
        char msg[24];
        StrCopy(msg, sizeof(msg), "0x");
        StrCat2(msg, sizeof(msg), msg, addrHex);
        StrCat2(msg, sizeof(msg), msg, "  -  ");
        StrCat2(msg, sizeof(msg), msg,
            s_results[s_cursor].scanned ? "NAK" : "NOT SCANNED");
        DrawText(px, py, msg, 1.2f,
            s_results[s_cursor].scanned ? D3DCOLOR_XRGB(55, 60, 85) : COL_DIM);
    }

    // ---- ID source badge (bottom-right of panel) ----------------------------
    {
        char badge[48];
        StrCopy(badge, sizeof(badge), "internal db");
        if (s_idFileLoaded && s_userKnownCount > 0)
        {
            StrCat2(badge, sizeof(badge), badge, "  +  smbid.id (");
            char cnt[6]; IntToStr(s_userKnownCount, cnt, sizeof(cnt));
            StrCat2(badge, sizeof(badge), badge, cnt);
            StrCat2(badge, sizeof(badge), badge, ")");
        }
        DrawTextR(PANEL_X + PANEL_W - 8.f, PANEL_BOTTOM - LINE_H - 2.f,
            badge, 1.0f, COL_DIM);
    }
}

// ============================================================================
// Tick
// ============================================================================

void SmBusScan_Tick(const DiagLogo& logo)
{
    WORD cur = GetButtons();

    if (EdgeDown(cur, s_prevBtns, BTN_B) || EdgeDown(cur, s_prevBtns, BTN_BACK))
    {
        if (s_regReadOpen)
            s_regReadOpen = false;
        else
        {
            RequestState(MSTATE_MENU);
            s_prevBtns = cur;
            return;
        }
    }
    else if (EdgeDown(cur, s_prevBtns, BTN_X))
    {
        ResetScan();
    }
    else if (s_regReadOpen)
    {
        // no extra input while register panel is open - [B] closes it (above)
    }
    else
    {
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_RIGHT) && s_cursor < ADDR_COUNT - 1)
            ++s_cursor;
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_LEFT) && s_cursor > 0)
            --s_cursor;
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_DOWN) && s_cursor + GRID_COLS < ADDR_COUNT)
            s_cursor += GRID_COLS;
        if (EdgeDown(cur, s_prevBtns, BTN_DPAD_UP) && s_cursor - GRID_COLS >= 0)
            s_cursor -= GRID_COLS;

        if (EdgeDown(cur, s_prevBtns, BTN_A) &&
            s_results[s_cursor].scanned &&
            s_results[s_cursor].ack)
        {
            OpenRegRead(s_cursor);
        }
    }

    s_prevBtns = cur;

    // Incremental scan - one address per frame
    if (s_phase == PHASE_SCANNING && s_scanNext < ADDR_COUNT)
    {
        BYTE dummy = 0;
        // HalReadSMBusValue uses 8-bit sw-shifted address (7-bit << 1)
        s_results[s_scanNext].ack = SMBusRead((BYTE)(s_scanNext << 1), 0x00, dummy);
        s_results[s_scanNext].scanned = true;
        if (++s_scanNext >= ADDR_COUNT)
            s_phase = PHASE_DONE;
    }

    // Render
    g_pDevice->BeginScene();

    const char* hint = s_regReadOpen
        ? "[B] Close registers    [X] Re-scan"
        : (s_phase == PHASE_SCANNING
            ? "[B] Back    [X] Re-scan"
            : "[A] Read registers    [B] Back    [X] Re-scan");

    DrawPageChrome(logo, "SMBUS SCAN", hint);
    DrawGrid();
    DrawProgressBar();
    DrawCursorStrip();
    DrawLegend();
    DrawInfoPanel();

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}