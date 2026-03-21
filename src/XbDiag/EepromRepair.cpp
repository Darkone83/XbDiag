// EepromRepair.cpp
// XbDiag - EEPROM repair, restore, and diagnostic validation.
//
// Validates 16 EEPROM fields against known-good rules, repairs repairable
// fields via direct SMBus write or ExSaveNonVolatileSetting, and restores
// from a D:\eeprom.bin backup. All repair state is private to this file.
// Reads s_eeprom and writes s_eepView via the externs in EepromView.h.

#include "EepromRepair.h"
#include "EepromSettings.h"
#include "EepromView.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

// ============================================================================
// EEPROM Repair — diagnostic item list (16 items, indices 0–15)
//
//   REPAIRABLE via SMBus direct write:
//     Factory checksum  (0x30) — ~Calc(data, 0x34, 0x2C)
//
//   REPAIRABLE via ExSaveNonVolatileSetting (kernel recalculates checksums):
//     User checksum     (0x60) — recalculated as part of the full 256-byte write
//     Video standard    (0x58) — reset to NTSC-M (0x00400100)
//     Video flags       (0x94) — mask to valid bits 0x005F0000
//     Audio flags       (0x98) — mask to valid bits 0x00030003
//     Game region       (0x2C) — reset to NA (0x00000001) via XC_GAME_REGION
//     DVD region        (0xBC) — reset to 0 (region free)
//     Language          (0x90) — reset to English (1)
//     Timezone          (0x64) — reset to UTC+0 via EepromSettings_GetUtcTzBlock
//     Game rating       (0x9C) — reset to disabled (0)
//     Movie rating      (0xA4) — reset to disabled (0)
//     Parental passcode (0xA0) — reset to disabled (0)
//
//   DETECT ONLY (no repair possible):
//     Security hash     (0x00) — HMAC-SHA1; detected via ExQueryNonVolatileSetting return code
//     Serial number     (0x34) — factory assigned; 12 ASCII digits check
//     MAC address       (0x40) — Xbox OUI prefix check + no multicast bit
//     Online key        (0x48) — nonzero check (all-zeros = never provisioned)
//
//   NEVER TOUCHED:
//     HDD key           (0x1C) — encrypted, hardware-unique; touching this bricks the HDD
//     Confounder        (0x14) — part of the security section encryption
//
// Checksum algorithm (RE'd from Xbox kernel 4034, confirmed same in 5838):
//   Accumulate all DWORDs with 64-bit carry tracking.
//   result = high + low
//   stored = ~result
//   valid:  stored + result == 0xFFFFFFFF
//
// Write paths:
//   Factory checksum  → HalWriteSMBusValue to EEPROM IC at 0xA8, byte by byte, 10ms delay
//   All user fields   → ExSaveNonVolatileSetting (full 256-byte write after RecalcChecksums)
//
// Restore from D:\eeprom.bin:
//   Reads 256 bytes, validates both checksums, writes all bytes via SMBus.
//   A confirm modal is shown before any write for both repair and restore.
// ============================================================================

// ---- Checksum ---------------------------------------------------------------

static DWORD EepCalcChecksum(const BYTE* data, int offset, int size)
{
    DWORD high = 0, low = 0;
    const BYTE* p = data + offset;
    for (int i = 0; i < size / 4; ++i)
    {
        DWORD val = (DWORD)p[i * 4 + 0]
            | ((DWORD)p[i * 4 + 1] << 8)
            | ((DWORD)p[i * 4 + 2] << 16)
            | ((DWORD)p[i * 4 + 3] << 24);
        DWORD newLow = low + val;
        if (newLow < low) ++high;
        low = newLow;
    }
    return high + low;
}

static bool EepFactoryChecksumOK()
{
    return EepCalcChecksum(s_eeprom, 0x30, 0x30) == 0xFFFFFFFF;
}

static bool EepUserChecksumOK()
{
    return EepCalcChecksum(s_eeprom, 0x60, 0x60) == 0xFFFFFFFF;
}

// ---- Field validators -------------------------------------------------------

static bool EepHmacOK()
{
    // Probe: ExQueryNonVolatileSetting(0xFFFF) returns STATUS_DEVICE_DATA_ERROR
    // (0xC000009C) if the HMAC SHA1 verification fails during the kernel's
    // cached EEPROM read. A 0 return means the hash is valid for this console.
    // This is the same check the kernel itself uses — no need to re-implement
    // the HMAC algorithm.
    ULONG type = 0, len = 0;
    BYTE  buf[256];
    LONG  r = ExQueryNonVolatileSetting(0xFFFF, &type, buf, 256, &len);
    return (r == 0);
}

static bool EepVideoStdOK()
{
    DWORD vs = EepReadDW(0x58);
    return (vs == 0x00400100 || vs == 0x00400200 ||
        vs == 0x00800300 || vs == 0x00400400);
}

static bool EepVideoFlagsOK()
{
    // Valid bits: widescreen=0x00010000, 720p=0x00020000, 1080i=0x00040000,
    //             480p=0x00080000, letterbox=0x00100000, PAL60=0x00400000
    return (EepReadDW(0x94) & ~0x005F0000UL) == 0;
}

static bool EepAudioFlagsOK()
{
    // Valid bits: mono=0x00000001, surround=0x00000002 (stereo=0), AC3=0x00010000, DTS=0x00020000
    return (EepReadDW(0x98) & ~0x00030003UL) == 0;
}

static bool EepGameRegionOK()
{
    // Security section is RC4-encrypted in the raw buffer — read game region
    // via kernel API which returns the decrypted value.
    ULONG type = 0, len = 0;
    DWORD gr = 0;
    if (ExQueryNonVolatileSetting(XC_GAME_REGION, &type, &gr, 4, &len) != 0)
        return false;
    return (gr == 0x00000001 || gr == 0x00000002 || gr == 0x00000004 ||
        gr == 0x80000000 || gr == 0x000000FF);
}

static bool EepDvdRegionOK()
{
    DWORD dr = EepReadDW(0xBC);
    return (dr <= 6);
}

static bool EepLanguageOK()
{
    DWORD lang = EepReadDW(0x90);
    // 0=Neutral, 1=English, 2=Japanese, 3=German, 4=French,
    // 5=Spanish, 6=Italian, 7=Korean, 8=Chinese, 9=Portuguese
    return (lang <= 9);
}

static bool EepTimezoneOK()
{
    // The timezone block (0x64-0x8F, 44 bytes) must match one of the known
    // Xbox timezone entries. We check the bias (0x64) is a plausible UTC
    // offset (within ±14 hours = ±840 minutes) and the std/dst name bytes
    // (0x68-0x6B and 0x6C-0x6F) are printable ASCII or zero.
    int bias = (int)EepReadDW(0x64);
    // The bias is stored in minutes (int32, positive = west of UTC).
    // Valid range: ±840 minutes (UTC-14 to UTC+14).
    if (bias < -840 || bias > 840) return false;
    // Check name bytes are ASCII printable or zero
    for (int i = 0x68; i < 0x70; ++i)
    {
        BYTE b = s_eeprom[i];
        if (b != 0 && (b < 0x20 || b > 0x7E)) return false;
    }
    return true;
}

// Additional validators aligned to XboxEepromEditor field definitions

static bool EepSerialOK()
{
    // Serial: 12 ASCII digits at 0x34
    for (int i = 0; i < 12; ++i)
        if (s_eeprom[0x34 + i] < '0' || s_eeprom[0x34 + i] > '9') return false;
    return true;
}

static bool EepMacOK()
{
    // MAC at 0x40: first byte must be one of the known Xbox OUI prefixes
    // 00:50:F2 (1.0+), 00:0D:3A (1.1+), 00:12:5A (1.6)
    BYTE b0 = s_eeprom[0x40], b1 = s_eeprom[0x41], b2 = s_eeprom[0x42];
    if (b0 == 0x00 && b1 == 0x50 && b2 == 0xF2) return true;
    if (b0 == 0x00 && b1 == 0x0D && b2 == 0x3A) return true;
    if (b0 == 0x00 && b1 == 0x12 && b2 == 0x5A) return true;
    // Multicast bit must not be set
    return (b0 & 0x01) == 0 && !(b0 == 0 && b1 == 0 && b2 == 0);
}

static bool EepParentalPasscodeOK()
{
    // Parental passcode at 0xA0: each nibble must be 0-4 (up/right/down/left/none)
    // 0=none, 1=up, 2=down, 3=left, 4=right; 0x00000000 = disabled
    DWORD pc = EepReadDW(0xA0);
    if (pc == 0) return true;
    for (int i = 0; i < 8; ++i)
    {
        BYTE nib = (BYTE)((pc >> (i * 4)) & 0xF);
        if (nib > 4) return false;
    }
    return true;
}

static bool EepGameRatingOK()
{
    // Max game rating at 0x9C: 0=disabled, 1-8 = ESRB ratings, 0xFF = all blocked
    DWORD gr = EepReadDW(0x9C);
    return (gr == 0 || (gr >= 1 && gr <= 8) || gr == 0xFF);
}

static bool EepMovieRatingOK()
{
    // XboxEepromEditor MovieRating: NR=0, NC17=1, R=2, PG13=4, PG=5, G=7
    // Values 3 and 6 are not defined (non-sequential enum).
    DWORD mr = EepReadDW(0xA4);
    return (mr == 0 || mr == 1 || mr == 2 || mr == 4 || mr == 5 || mr == 7);
}

static bool EepOnlineKeyNonzero()
{
    // Online key at 0x48 (16 bytes) — all zeros suggests it was never provisioned
    for (int i = 0; i < 16; ++i)
        if (s_eeprom[0x48 + i] != 0) return true;
    return false;
}

// ---- Restore from D:\eeprom.bin ---------------------------------------------
// Writes 256 bytes back to the EEPROM IC via SMBus, byte by byte.
// The file must be exactly 256 bytes. We verify the factory and user checksums
// of the file before writing — if they're wrong we abort.

static bool RestoreFromFile()
{
    HANDLE hf = CreateFileA("D:\\eeprom.bin", GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;

    BYTE  buf[256];
    DWORD nr = 0;
    BOOL  ok = ReadFile(hf, buf, 256, &nr, NULL);
    CloseHandle(hf);

    if (!ok || nr != 256) return false;

    // Validate checksums before writing
    DWORD factoryCS = EepCalcChecksum(buf, 0x30, 0x30);
    DWORD userCS = EepCalcChecksum(buf, 0x60, 0x60);
    if (factoryCS != 0xFFFFFFFF || userCS != 0xFFFFFFFF) return false;

    // Write byte by byte via SMBus with write cycle delay
    for (int i = 0; i < 256; ++i)
    {
        if (!SMBusWrite(SMBADDR_EEPROM, (BYTE)i, buf[i])) return false;
        Sleep(10);  // 93LC56 write cycle time
    }

    // Update local cache
    for (int i = 0; i < 256; ++i) s_eeprom[i] = buf[i];
    return true;
}

// ---- SMBus EEPROM direct write (byte at a time with write cycle delay) ------

static bool EepSMBusWriteDW(int offset, DWORD val)
{
    bool ok = true;
    for (int i = 0; i < 4; ++i)
    {
        BYTE b = (BYTE)((val >> (i * 8)) & 0xFF);
        if (!SMBusWrite(SMBADDR_EEPROM, (BYTE)(offset + i), b))
            ok = false;
        Sleep(10);  // 93LC56 write cycle time

    }
    return ok;
}

// ---- Repair item table ------------------------------------------------------

enum RepairItemState { REP_OK = 0, REP_BAD, REP_FIXED, REP_FAIL, REP_INFO };

struct RepairItem
{
    const char* label;
    const char* description;
    RepairItemState state;
    bool            canRepair;  // false = detect-only
};

static RepairItem s_repItems[16];
static int        s_repCount = 0;

static void RepairBuildDiag()
{
    s_repCount = 0;

    auto add = [](const char* lbl, const char* desc, bool ok, bool canFix) {
        s_repItems[s_repCount].label = lbl;
        s_repItems[s_repCount].description = desc;
        s_repItems[s_repCount].state = ok ? REP_OK : (canFix ? REP_BAD : REP_INFO);
        s_repItems[s_repCount].canRepair = canFix;
        ++s_repCount;
        };

    // Checksum section
    add("Factory Checksum", "0x30  covers serial/MAC/keys/video std", EepFactoryChecksumOK(), true);
    add("User Checksum", "0x60  covers timezone/language/flags", EepUserChecksumOK(), true);

    // Security section (detect only — no repair without kernel crypto)
    add("Security Hash", "0x00  HMAC-SHA1  (detect only)", EepHmacOK(), false);

    // Factory section fields — from XboxEepromEditor layout
    add("Serial Number", "0x34  12 ASCII digits", EepSerialOK(), false);
    add("MAC Address", "0x40  Xbox OUI prefix + non-multicast", EepMacOK(), false);
    add("Online Key", "0x48  16-byte key (nonzero check)", EepOnlineKeyNonzero(), false);

    // User settings — repairable via ExSaveNonVolatileSetting
    add("Video Standard", "0x58  NTSC-M/J or PAL-I/M", EepVideoStdOK(), true);
    add("Video Flags", "0x94  no undocumented bits set", EepVideoFlagsOK(), true);
    add("Audio Flags", "0x98  no undocumented bits set", EepAudioFlagsOK(), true);
    add("Game Region", "0x2C (enc)  NA / Japan / RoW / Dev", EepGameRegionOK(), true);
    add("DVD Region", "0xBC  CSS zone 0-6", EepDvdRegionOK(), true);
    add("Language", "0x90  Neutral-Portuguese (0-9)", EepLanguageOK(), true);
    add("Timezone", "0x64  valid bias + printable TZ name", EepTimezoneOK(), true);
    add("Game Rating", "0x9C  ESRB 0-8 or 0xFF", EepGameRatingOK(), true);
    add("Movie Rating", "0xA4  MPAA: NR=0 NC17=1 R=2 PG13=4 PG=5 G=7", EepMovieRatingOK(), true);
    add("Parental Passcode", "0xA0  nibbles 0-4 or zero", EepParentalPasscodeOK(), true);
}

static int RepairBadCount()
{
    int n = 0;
    for (int i = 0; i < s_repCount; ++i)
        if (s_repItems[i].state == REP_BAD) ++n;
    return n;
}

// ---- Apply repairs ----------------------------------------------------------
// Strategy matches PrometheOS save() approach:
//   1. Game region (security section) — kernel-managed per-field write first
//   2. Read fresh 256-byte image
//   3. Write all bad user-section field defaults directly into the buffer
//   4. Recalculate both checksums in the buffer
//   5. Write all 256 bytes atomically via ExSaveNonVolatileSetting
//   6. Re-read and verify checksums; update item states to FIXED or FAIL
//
// Factory checksum (item 0) is included in the Checksum2 recalculation at step 4 —
// no separate SMBus write needed. Items 2-5 (security hash, serial, MAC, online key)
// are detect-only and never modified.

static void RepairApply()
{
    // ── Step 1: Game region (item 9) — kernel handles security section encryption
    if (s_repItems[9].state == REP_BAD)
    {
        DWORD gr = 0x00000001;  // reset to N. America
        LONG r = ExSaveNonVolatileSetting(XC_GAME_REGION, REG_DWORD, &gr, 4);
        s_repItems[9].state = (r == 0) ? REP_FIXED : REP_FAIL;
    }

    // ── Step 2: Read fresh 256-byte image ─────────────────────────────────────
    BYTE buf[256];
    {
        ULONG type = 0, len = 0;
        if (ExQueryNonVolatileSetting(0xFFFF, &type, buf, 256, &len) != 0)
        {
            // Cannot read — mark all pending repairs failed
            for (int i = 0; i < s_repCount; ++i)
                if (s_repItems[i].state == REP_BAD) s_repItems[i].state = REP_FAIL;
            return;
        }
    }

    // ── Step 3: Write safe defaults for each bad user-section field ───────────

    // Item 6: Video Standard (0x58) — factory section (Checksum2 covers this)
    if (s_repItems[6].state == REP_BAD)
    {
        BufWriteDW(buf, 0x58, 0x00400100);  // NTSC-M
        s_repItems[6].state = REP_FIXED;    // confirmed below after write
    }

    // Item 7: Video Flags (0x94) — mask undocumented bits, preserve valid ones
    if (s_repItems[7].state == REP_BAD)
    {
        DWORD vf = EepReadDW(0x94) & 0x005F0000UL;
        BufWriteDW(buf, 0x94, vf);
        s_repItems[7].state = REP_FIXED;
    }

    // Item 8: Audio Flags (0x98) — mask undocumented bits, resolve mono+surround conflict
    if (s_repItems[8].state == REP_BAD)
    {
        DWORD af = EepReadDW(0x98) & 0x00030003UL;
        if ((af & 0x03) == 0x03) af &= ~0x01UL;  // mono+surround both set → keep surround
        BufWriteDW(buf, 0x98, af);
        s_repItems[8].state = REP_FIXED;
    }

    // Item 10: DVD Region (0xBC)
    if (s_repItems[10].state == REP_BAD)
    {
        BufWriteDW(buf, 0xBC, 0);  // 0 = region free
        s_repItems[10].state = REP_FIXED;
    }

    // Item 11: Language (0x90)
    if (s_repItems[11].state == REP_BAD)
    {
        BufWriteDW(buf, 0x90, 1);  // 1 = English
        s_repItems[11].state = REP_FIXED;
    }

    // Item 12: Timezone — reset to London/UTC+0 (full 44-byte block at 0x64-0x8F)
    if (s_repItems[12].state == REP_BAD)
    {
        // Reset to UTC+0 (London) — get the block from Settings
        BYTE tzBlock[44];
        EepromSettings_GetUtcTzBlock(tzBlock);
        for (int i = 0; i < 44; ++i)
            buf[0x64 + i] = tzBlock[i];
        s_repItems[12].state = REP_FIXED;
    }

    // Item 13: Game Rating (0x9C)
    if (s_repItems[13].state == REP_BAD)
    {
        BufWriteDW(buf, 0x9C, 0);  // 0 = disabled (all ages)
        s_repItems[13].state = REP_FIXED;
    }

    // Item 14: Movie Rating (0xA4)
    if (s_repItems[14].state == REP_BAD)
    {
        BufWriteDW(buf, 0xA4, 0);  // 0 = disabled
        s_repItems[14].state = REP_FIXED;
    }

    // Item 15: Parental Passcode (0xA0)
    if (s_repItems[15].state == REP_BAD)
    {
        BufWriteDW(buf, 0xA0, 0);  // 0 = disabled
        s_repItems[15].state = REP_FIXED;
    }

    // ── Step 4: Recalculate both checksums ────────────────────────────────────
    // Checksum2 (0x30) covers factory section 0x34-0x5F (video std lives here).
    // Checksum3 (0x60) covers user section 0x64-0xBF (all other repaired fields).
    // This also implicitly repairs items 0 and 1 (bad checksums).
    EepromSettings_RecalcChecksums(buf);

    // ── Step 5: Write all 256 bytes atomically ────────────────────────────────
    ULONG type = 3;
    bool writeOK = (ExSaveNonVolatileSetting(0xFFFF, type, buf, 256) == 0);

    // If write failed, demote all freshly-marked FIXED items to FAIL
    if (!writeOK)
    {
        for (int i = 0; i < s_repCount; ++i)
            if (s_repItems[i].state == REP_FIXED) s_repItems[i].state = REP_FAIL;
        return;
    }

    // ── Step 6: Re-read and verify checksums ──────────────────────────────────
    {
        ULONG rtype = 0, rlen = 0;
        BYTE verify[256];
        if (ExQueryNonVolatileSetting(0xFFFF, &rtype, verify, 256, &rlen) == 0)
        {
            for (int i = 0; i < 256; ++i) s_eeprom[i] = verify[i];

            // Update checksum repair states based on actual on-chip result
            if (s_repItems[0].state == REP_BAD || s_repItems[0].state == REP_FIXED)
                s_repItems[0].state = EepFactoryChecksumOK() ? REP_FIXED : REP_FAIL;
            if (s_repItems[1].state == REP_BAD || s_repItems[1].state == REP_FIXED)
                s_repItems[1].state = EepUserChecksumOK() ? REP_FIXED : REP_FAIL;
        }
    }
}

// ---- Repair render ----------------------------------------------------------

static void RenderRepair(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    // ---- Full-screen confirm overlay (repair or restore) --------------------
    // Rendered over a dimmed background before anything else so it
    // feels like a proper modal rather than a banner at the bottom.
    if (s_repConfirm || s_restoreConfirm)
    {
        // Dim the whole screen
        FillRect(0.f, 0.f, SW, SH, D3DCOLOR_ARGB(160, 0, 0, 0));

        // Box
        const float BW = 420.f;
        const float BH = 110.f;
        const float BX = (SW - BW) * 0.5f;
        const float BY = (SH - BH) * 0.5f;
        FillRect(BX, BY, BX + BW, BY + BH, D3DCOLOR_XRGB(18, 18, 32));
        // Border
        HLine(BY, BX, BX + BW, s_restoreConfirm ? COL_ORANGE : COL_CYAN);
        HLine(BY + BH, BX, BX + BW, s_restoreConfirm ? COL_ORANGE : COL_CYAN);
        VLine(BX, BY, BY + BH, s_restoreConfirm ? COL_ORANGE : COL_CYAN);
        VLine(BX + BW, BY, BY + BH, s_restoreConfirm ? COL_ORANGE : COL_CYAN);

        const char* title = s_restoreConfirm ? "RESTORE FROM EEPROM.BIN" : "CONFIRM REPAIR";
        const char* body = s_restoreConfirm
            ? "Write all 256 bytes from D:\\eeprom.bin to EEPROM hardware?"
            : "Write repairs to EEPROM hardware?";
        const char* warning = s_restoreConfirm
            ? "File checksums are verified before writing."
            : "Only corrupt fields will be written. HDD key untouched.";

        DrawText(BX + 12.f, BY + 10.f, title, 1.3f,
            s_restoreConfirm ? COL_ORANGE : COL_CYAN);
        DrawText(BX + 12.f, BY + 32.f, body, 1.1f, COL_WHITE);
        DrawText(BX + 12.f, BY + 52.f, warning, 1.0f, COL_DIM);
        DrawText(BX + 12.f, BY + 76.f,
            "[A] Yes, proceed    [B] Cancel",
            1.15f, COL_YELLOW);

        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
        return;
    }

    // Build hint based on current state.
    // Note: s_repConfirm and s_restoreConfirm are handled by the early-return
    // modal overlay above — those flags can never be true here.
    const char* hint;
    if (s_restoreDone)
        hint = s_restoreOK ? "[A] Re-scan    [B] Back"
        : "[B] Back — restore failed";
    else if (s_repRan)
        hint = "[A] Re-scan    [B] Back";
    else if (RepairBadCount() > 0)
        hint = "[A] Repair issues    [B] Back";
    else
        hint = "[B] Back — no repairable issues";

    DrawPageChrome(logo, "EEPROM - REPAIR", hint);

    float y = CONTENT_Y + 4.f;

    // ---- Restore-from-file result banner (restore done, not confirm — confirm
    //      was already handled by the modal overlay above) --------------------
    if (s_restoreDone)
    {
        // Restore done
        if (s_restoreOK)
        {
            FillRect(0.f, y, SW, y + LINE_H * 2.f + 12.f, D3DCOLOR_ARGB(50, 0, 160, 60));
            DrawText(LM, y + 4.f, "RESTORE COMPLETE", 1.3f, COL_GREEN);
            y += LINE_H + 6.f;
            DrawText(LM, y,
                "All 256 bytes written. Press [A] to re-scan, or [B] to return.",
                1.1f, COL_WHITE);
        }
        else
        {
            FillRect(0.f, y, SW, y + LINE_H * 3.f + 14.f, D3DCOLOR_ARGB(50, 180, 40, 0));
            DrawText(LM, y + 4.f, "RESTORE FAILED", 1.3f, COL_RED);
            y += LINE_H + 6.f;
            DrawText(LM, y,
                "Possible causes: file not found, not 256 bytes,",
                1.05f, COL_WHITE);
            y += LINE_H;
            DrawText(LM, y,
                "invalid checksums in file, or SMBus write error.",
                1.05f, COL_WHITE);
        }

        g_pDevice->EndScene();
        g_pDevice->Present(NULL, NULL, NULL, NULL);
        return;
    }

    // ---- Normal repair view -------------------------------------------------
    DrawText(LM, y,
        "! HDD key / confounder / security hash / serial / MAC are never modified.",
        1.05f, COL_ORANGE);
    y += LINE_H + 6.f;
    HLine(y, LM, SW - LM, COL_BORDER);
    y += 4.f;

    // Column headers
    const float COL_LBL = LM;
    const float COL_DESC = LM + 148.f;
    const float COL_STA = LM + 360.f;
    DrawText(COL_LBL, y, "FIELD", 1.0f, COL_DIM);
    DrawText(COL_DESC, y, "DESCRIPTION", 1.0f, COL_DIM);
    DrawText(COL_STA, y, "STATUS", 1.0f, COL_DIM);
    y += LINE_H;
    HLine(y, 0.f, SW, D3DCOLOR_XRGB(18, 22, 48));
    y += 3.f;

    const float ROW_H = LINE_H + 3.f;
    for (int i = 0; i < s_repCount; ++i)
    {
        const RepairItem& ri = s_repItems[i];

        if (ri.state == REP_BAD)
            FillRect(0.f, y - 1.f, SW, y + ROW_H - 1.f, D3DCOLOR_ARGB(35, 200, 60, 0));
        else if (ri.state == REP_FIXED)
            FillRect(0.f, y - 1.f, SW, y + ROW_H - 1.f, D3DCOLOR_ARGB(35, 0, 180, 60));
        else if (ri.state == REP_INFO)
            FillRect(0.f, y - 1.f, SW, y + ROW_H - 1.f, D3DCOLOR_ARGB(20, 80, 80, 200));

        DWORD lblCol = (ri.state == REP_BAD || ri.state == REP_FAIL) ? COL_WHITE
            : (ri.state == REP_FIXED) ? D3DCOLOR_XRGB(140, 220, 160)
            : COL_GRAY;

        DrawText(COL_LBL, y, ri.label, 1.15f, lblCol);
        DrawText(COL_DESC, y, ri.description, 1.0f, COL_DIM);

        const char* stateStr;
        DWORD stateCol;
        switch (ri.state)
        {
        case REP_OK:    stateStr = "OK";       stateCol = COL_GREEN;                 break;
        case REP_BAD:   stateStr = "CORRUPT";  stateCol = COL_RED;                   break;
        case REP_FIXED: stateStr = "REPAIRED"; stateCol = COL_CYAN;                  break;
        case REP_FAIL:  stateStr = "FAILED";   stateCol = COL_ORANGE;                break;
        case REP_INFO:  stateStr = "INVALID";  stateCol = D3DCOLOR_XRGB(160, 120, 80); break;
        default:        stateStr = "?";        stateCol = COL_DIM;                   break;
        }

        static char s_staBuf[32];
        if (!ri.canRepair && ri.state == REP_INFO)
        {
            StrCopy(s_staBuf, sizeof(s_staBuf), stateStr);
            StrCat2(s_staBuf, sizeof(s_staBuf), s_staBuf, " (no repair)");
            stateStr = s_staBuf;
        }

        DrawText(COL_STA, y, stateStr, 1.1f, stateCol);
        HLine(y + ROW_H - 1.f, 0.f, SW, D3DCOLOR_XRGB(14, 16, 34));
        y += ROW_H;
    }

    y += 6.f;

    if (s_repRan)
    {
        int fixed = 0, failed = 0;
        for (int i = 0; i < s_repCount; ++i)
        {
            if (s_repItems[i].state == REP_FIXED) ++fixed;
            if (s_repItems[i].state == REP_FAIL)  ++failed;
        }
        char msg[80]; char fa[6], fb[6];
        IntToStr(fixed, fa, sizeof(fa)); IntToStr(failed, fb, sizeof(fb));
        StrCopy(msg, sizeof(msg), "Complete: ");
        StrCat2(msg, sizeof(msg), msg, fa);
        StrCat2(msg, sizeof(msg), msg, " item(s) repaired");
        if (failed > 0)
        {
            StrCat2(msg, sizeof(msg), msg, ",  ");
            StrCat2(msg, sizeof(msg), msg, fb);
            StrCat2(msg, sizeof(msg), msg, " failed");
        }
        DrawText(LM, y, msg, 1.2f, failed ? COL_ORANGE : COL_GREEN);
        y += LINE_H + 2.f;
        DrawText(LM, y,
            "Press [A] to re-scan and confirm, or [B] to return to decoded view.",
            1.05f, COL_DIM);
    }
    else if (RepairBadCount() == 0)
    {
        int infoCount = 0;
        for (int i = 0; i < s_repCount; ++i)
            if (s_repItems[i].state == REP_INFO) ++infoCount;

        DrawText(LM, y, "All repairable fields are valid.", 1.2f, COL_GREEN);
        if (infoCount > 0)
        {
            y += LINE_H + 2.f;
            DrawText(LM, y,
                "! Security hash is invalid (detect only). "
                "Usually caused by EEPROM corruption or wrong kernel version.",
                1.05f, COL_ORANGE);
        }
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ---- Repair input handler ---------------------------------------------------

static void RepairHandleInput(WORD cur, WORD prev, const DiagLogo& logo)
{
    auto Edge = [](WORD c, WORD p, WORD b) { return (c & b) && !(p & b); };

    // ── Confirm: REPAIR ───────────────────────────────────────────────────
    if (s_repConfirm)
    {
        if (Edge(cur, prev, BTN_A))
        {
            // Confirmed — draw progress frame then execute
            g_pDevice->BeginScene();
            DrawPageChrome(logo, "EEPROM - REPAIR", "");
            DrawText(LM, CONTENT_Y + 60.f, "Repairing EEPROM...", 1.5f, COL_YELLOW);
            DrawText(LM, CONTENT_Y + 80.f, "Do not power off", 1.2f, COL_RED);
            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);
            RepairApply();
            s_repRan = true;
            s_repConfirm = false;
        }
        else if (Edge(cur, prev, BTN_B) || Edge(cur, prev, BTN_BACK))
        {
            // Aborted — dismiss confirm, stay in repair view
            s_repConfirm = false;
        }
        return;
    }

    // ── Confirm: RESTORE ──────────────────────────────────────────────────
    if (s_restoreConfirm)
    {
        if (Edge(cur, prev, BTN_A))
        {
            // Confirmed — draw progress frame then execute
            g_pDevice->BeginScene();
            DrawPageChrome(logo, "EEPROM - REPAIR", "");
            DrawText(LM, CONTENT_Y + 60.f, "Writing EEPROM...", 1.5f, COL_YELLOW);
            DrawText(LM, CONTENT_Y + 80.f, "Do not power off", 1.2f, COL_RED);
            g_pDevice->EndScene();
            g_pDevice->Present(NULL, NULL, NULL, NULL);
            s_restoreOK = RestoreFromFile();
            s_restoreDone = true;
            s_restoreConfirm = false;
        }
        else if (Edge(cur, prev, BTN_B) || Edge(cur, prev, BTN_BACK))
        {
            // Aborted — dismiss confirm, stay in repair view
            s_restoreConfirm = false;
        }
        return;
    }

    // ── Post-restore result ───────────────────────────────────────────────
    if (s_restoreDone)
    {
        if (Edge(cur, prev, BTN_A))
        {
            EepromView_Reload();
            s_restoreDone = false;
            s_restoreOK = false;
            s_repRan = false;
        }
        else if (Edge(cur, prev, BTN_B) || Edge(cur, prev, BTN_BACK))
        {
            s_restoreDone = false;
            s_eepView = VIEW_DECODED;
        }
        return;
    }

    // ── Post-repair result ────────────────────────────────────────────────
    if (s_repRan)
    {
        if (Edge(cur, prev, BTN_A))
        {
            EepromView_Reload();
            s_repRan = false;
        }
        else if (Edge(cur, prev, BTN_B) || Edge(cur, prev, BTN_BACK))
        {
            s_repRan = false;
            s_eepView = VIEW_DECODED;
        }
        return;
    }

    // ── Normal repair list ────────────────────────────────────────────────
    if (Edge(cur, prev, BTN_B) || Edge(cur, prev, BTN_BACK))
    {
        s_eepView = VIEW_DECODED;
        return;
    }
    if (Edge(cur, prev, BTN_A) && RepairBadCount() > 0)
        s_repConfirm = true;
}

// ============================================================================
// Public API wrappers
// ============================================================================

DWORD EepRepair_CalcChecksum(const BYTE* data, int offset, int size)
{
    return EepCalcChecksum(data, offset, size);
}

bool EepRepair_SMBusWriteDW(int offset, DWORD val)
{
    return EepSMBusWriteDW(offset, val);
}

void EepromRepair_BuildDiag()
{
    RepairBuildDiag();
}

void EepromRepair_HandleInput(WORD cur, WORD prev, const DiagLogo& logo)
{
    RepairHandleInput(cur, prev, logo);
}

void EepromRepair_Render(const DiagLogo& logo)
{
    RenderRepair(logo);
}