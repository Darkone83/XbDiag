// HddSmartDB.hpp
// XbDiag - SMART vendor attribute database.
//
// Self-contained — include only from HddSmart.cpp.
// All data is static const (lands in .rdata, not BSS).
// All functions are static inline (no separate translation unit).
//
// Lookup strategy:
//   1. Vendor prefix table  — prefix match on model string, first match wins
//   2. Default attribute table — de facto standard meanings
//   3. Fallback               — "Attr 0xXX" + RF_HEX
//
// SSD overrides are organised by controller family since most consumer SSDs
// share vendor-specific attribute IDs within a controller family regardless
// of brand name. Brand prefixes in k_vendors[] alias to the same table.
//
// RXDK constraints: no CRT, no dynamic allocation, C89 declarations.

#pragma once
#include "DiagCommon.h"

// ============================================================================
// Raw format enum
// ============================================================================

enum RawFmt
{
    RF_HEX,          // 6-byte hex dump — fallback / genuinely opaque values
    RF_COUNT32,      // lower 32 bits as plain decimal
    RF_HOURS,        // lower 24 bits as "8760h"  (raw24(raw8) — matches smartmontools)
    RF_MINUTES,      // lower 32 bits in minutes -> "146h 0m"  (min2hour)
    RF_SECONDS,      // lower 32 bits in seconds -> "2h 3m 4s"  (sec2hour)
    RF_TEMP,         // byte[0]=current°C, bytes[2/3]=min/max with sanity gate
    RF_SECTORS,      // lower 16 bits as sector count
    RF_PERCENT,      // byte[0] as "87%"
    RF_RAW16,        // lower 16 bits as decimal (Seagate error rate style)
    RF_RAW16_OPT16,  // lower 16 bits primary, upper 16 in parens if nonzero (raw16(raw16))
    RF_RAW16_AVG16,  // lower 16 bits as ms, upper 16 as "avg:X" if nonzero (raw16(avg16))
    RF_GIB           // lower 32 bits as "N GiB"
};

// ============================================================================
// Attribute definition
// ============================================================================

struct SmartAttrDef
{
    BYTE        id;
    const char* name;
    RawFmt      fmt;
    bool        critical;
};

// ============================================================================
// Vendor entry
// ============================================================================

struct SmartVendorEntry
{
    const char* prefix;
    const SmartAttrDef* attrs;
    int                 attrCount;
};

// ============================================================================
// Default attribute table
// Only IDs with stable, widely-agreed meaning across vendors.
// Intentionally conservative — uncertain attrs use RF_HEX.
// ============================================================================

static const SmartAttrDef k_defaultAttrs[] =
{
    { 0x01, "Read Error Rate",    RF_HEX,          false },  // vendor-specific raw
    { 0x02, "Throughput Perf",    RF_HEX,          false },
    { 0x03, "Spin-Up Time",       RF_RAW16_AVG16,  false },  // ms, optional avg (raw16(avg16))
    { 0x04, "Start/Stop Count",   RF_COUNT32,      false },
    { 0x05, "Reallocated Sects",  RF_RAW16_OPT16,  true  },  // raw16(raw16) — critical
    { 0x07, "Seek Error Rate",    RF_HEX,          false },  // vendor-specific raw
    { 0x08, "Seek Time Perf",     RF_HEX,          false },
    { 0x09, "Power-On Hours",     RF_HOURS,        false },  // raw24(raw8)
    { 0x0A, "Spin Retry Count",   RF_COUNT32,      false },
    { 0x0B, "Calibration Retry",  RF_COUNT32,      false },
    { 0x0C, "Power Cycle Count",  RF_COUNT32,      false },
    { 0x0D, "Soft Read Err Rate", RF_HEX,          false },
    { 0x16, "Helium Level",       RF_COUNT32,      false },  // HGST/WD He drives
    { 0xA8, "SATA PHY Errors",    RF_COUNT32,      true  },
    { 0xB7, "Runtime Bad Block",  RF_COUNT32,      false },
    { 0xB8, "End-to-End Error",   RF_COUNT32,      true  },
    { 0xBB, "Uncorr ECC Count",   RF_COUNT32,      true  },
    { 0xBC, "Command Timeout",    RF_COUNT32,      false },
    { 0xBD, "High Fly Writes",    RF_COUNT32,      true  },
    { 0xBE, "Airflow Temp",       RF_TEMP,         false },
    { 0xBF, "G-Sense Errors",     RF_COUNT32,      false },
    { 0xC0, "Unsafe Shutdowns",   RF_COUNT32,      false },
    { 0xC1, "Load/Unload Cycles", RF_COUNT32,      false },
    { 0xC2, "Temperature",        RF_TEMP,         false },
    { 0xC3, "HW ECC Recovered",   RF_HEX,          false },
    { 0xC4, "Realloc Event Cnt",  RF_RAW16_OPT16,  false }, // raw16(raw16)
    { 0xC5, "Pending Sectors",    RF_COUNT32,      true  },
    { 0xC6, "Uncorrect Sects",    RF_COUNT32,      true  },
    { 0xC7, "UDMA CRC Errors",    RF_COUNT32,      true  },
    { 0xC8, "Write Error Rate",   RF_HEX,          false },
    { 0xC9, "Soft Read Err Rate", RF_HEX,          false },
    { 0xCA, "Data Addr Mk Errs",  RF_COUNT32,      false },
    { 0xCB, "Run Out Cancel",     RF_COUNT32,      false },
    { 0xCC, "Soft ECC Correct",   RF_COUNT32,      false },
    { 0xCD, "Thermal Asperity",   RF_COUNT32,      false },
    { 0xDE, "Loaded Hours",       RF_HOURS,        false },
    { 0xF0, "Head Flying Hours",  RF_HOURS,        false },
    { 0xF1, "Total LBA Written",  RF_HEX,          false },  // unit varies by vendor
    { 0xF2, "Total LBA Read",     RF_HEX,          false },  // unit varies by vendor
    { 0xFE, "Free Fall Protect",  RF_COUNT32,      false },
};

static const int k_defaultAttrCount =
(int)(sizeof(k_defaultAttrs) / sizeof(k_defaultAttrs[0]));

// ============================================================================
// Vendor / controller override tables
// ============================================================================

// --- Seagate (all families: Barracuda, IronWolf, Exos, SkyHawk, Pipeline) ---
// 0x01/0x07/0xC8: lower 16 bits = actual error count, upper = total operations
static const SmartAttrDef k_seagate[] =
{
    { 0x01, "Read Error Rate",  RF_RAW16,   false },
    { 0x07, "Seek Error Rate",  RF_RAW16,   false },
    { 0xC8, "Write Error Rate", RF_RAW16,   false },
};

// --- Western Digital HDD (Caviar / Blue / Green / Red / Purple / Black / Gold)
// WD follows defaults cleanly — no overrides needed
static const SmartAttrDef k_wd_hdd[] =
{
    { 0x00, NULL, RF_HEX, false }  // sentinel — zero overrides
};

// --- Samsung HDD SpinPoint (SP / HD series — OEM Xbox era) ------------------
static const SmartAttrDef k_samsung_hdd[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },  // straight hours
    { 0xC1, "Load/Unload Cycles", RF_COUNT32, false },
};

// --- IBM Deskstar / Hitachi Deskstar early (DTLA / IC35) --------------------
static const SmartAttrDef k_ibm[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
    { 0xC1, "Load/Unload Cycles", RF_COUNT32, false },
};

// --- Hitachi / HGST (HDS / HUA / HTE / HDT / H7xxx) -----------------------
static const SmartAttrDef k_hitachi[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
    { 0xC1, "Load/Unload Cycles", RF_COUNT32, false },
    { 0xBE, "Airflow Temp",       RF_TEMP,    false },
};

// --- Toshiba 2.5" MK series — power-on time in minutes ---------------------
static const SmartAttrDef k_toshiba_mk[] =
{
    { 0x09, "Power-On Minutes",   RF_MINUTES, false },
    { 0xBE, "Airflow Temp",       RF_TEMP,    false },
    { 0xC1, "Load/Unload Cycles", RF_COUNT32, false },
};

// --- Toshiba 3.5" DT / MD / MG / S300 — power-on time in hours ------------
static const SmartAttrDef k_toshiba_35[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
    { 0xBE, "Airflow Temp",       RF_TEMP,    false },
    { 0xC1, "Load/Unload Cycles", RF_COUNT32, false },
};

// --- Fujitsu (MHT / MHV / MHS / MHY / MHZ — original Xbox OEM) ------------
static const SmartAttrDef k_fujitsu[] =
{
    { 0x01, "Read Error Rate",    RF_COUNT32, false },
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
    { 0xBE, "Airflow Temp",       RF_TEMP,    false },
};

// --- Maxtor (DiamondMax 6Y / 6L / 7Y / 7L — OEM Xbox era) -----------------
static const SmartAttrDef k_maxtor[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
    { 0xC8, "Write Error Rate",   RF_COUNT32, false },
    { 0xC9, "Soft Read Err Rate", RF_COUNT32, false },
};

// --- Quantum Fireball (pre-Maxtor acquisition) ------------------------------
static const SmartAttrDef k_quantum[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
};

// --- Samsung SSD (840 / 850 / 860 / 870 EVO/PRO, PM series) ----------------
static const SmartAttrDef k_samsung_ssd[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
    { 0xAF, "Program Fail Count", RF_COUNT32, false },
    { 0xB0, "Erase Fail Count",   RF_COUNT32, false },
    { 0xB1, "Wear Level Count",   RF_COUNT32, false },
    { 0xB2, "Used Reserve Blk",   RF_COUNT32, false },
    { 0xB4, "Unused Reserve Blk", RF_COUNT32, false },
    { 0xBB, "Uncorr Error Cnt",   RF_COUNT32, true  },
    { 0xBE, "Airflow Temp",       RF_TEMP,    false },
    { 0xC2, "Temperature",        RF_TEMP,    false },
    { 0xC7, "CRC Error Count",    RF_COUNT32, false },
    { 0xEB, "POR Recovery Cnt",   RF_COUNT32, false },
    { 0xF1, "Total Writes GiB",   RF_GIB,     false },
    { 0xF2, "Total Reads GiB",    RF_GIB,     false },
};

// --- Intel SSD (SSDSA / SSDSC / SSDSD — own controller) --------------------
static const SmartAttrDef k_intel_ssd[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
    { 0xC0, "Unsafe Shutdowns",   RF_COUNT32, false },
    { 0xE1, "Host Writes 32MiB",  RF_COUNT32, false },
    { 0xE2, "Timer 32MiB",        RF_COUNT32, false },
    { 0xE8, "Available Reserve",  RF_PERCENT, false },
    { 0xE9, "Media Wearout Ind",  RF_COUNT32, false },
    { 0xF1, "Host Writes",        RF_GIB,     false },
    { 0xF2, "Host Reads",         RF_GIB,     false },
};

// --- Crucial / Micron (BX / MX / M500 — Marvell / Silicon Motion) ----------
static const SmartAttrDef k_crucial[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
    { 0xAA, "Available Reserve",  RF_COUNT32, false },
    { 0xAB, "Program Fail Cnt",   RF_COUNT32, false },
    { 0xAC, "Erase Fail Count",   RF_COUNT32, false },
    { 0xAD, "Wear Level Count",   RF_COUNT32, false },
    { 0xAE, "Unexpect Poweroff",  RF_COUNT32, false },
    { 0xC2, "Temperature",        RF_TEMP,    false },
    { 0xCA, "Perc Lifetime Used", RF_PERCENT, false },
    { 0xF1, "Total Writes GiB",   RF_GIB,     false },
    { 0xF2, "Total Reads GiB",    RF_GIB,     false },
};

// --- SandForce controller (OCZ / Kingston HyperX / Corsair Force / ADATA SP)
// Many brands use SandForce SF-1200/SF-2281 with identical vendor attrs
static const SmartAttrDef k_sandforce[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
    { 0xAB, "Program Fail Cnt",   RF_COUNT32, false },
    { 0xAC, "Erase Fail Count",   RF_COUNT32, false },
    { 0xAE, "Unexpect Poweroff",  RF_COUNT32, false },
    { 0xB5, "Program Fail Total", RF_COUNT32, false },
    { 0xC2, "Temperature",        RF_TEMP,    false },
    { 0xE6, "Life Curve Status",  RF_COUNT32, false },
    { 0xE7, "SSD Life Left",      RF_PERCENT, false },
    { 0xE8, "Available Reserve",  RF_PERCENT, false },
    { 0xE9, "Media Wearout Ind",  RF_COUNT32, false },
    { 0xF1, "Lifetime Writes GiB",RF_GIB,     false },
    { 0xF2, "Lifetime Reads GiB", RF_GIB,     false },
};

// --- Phison controller (Kingston A400/UV400/UV500, PNY CS, Patriot Burst) ---
static const SmartAttrDef k_phison[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
    { 0xAA, "Available Reserve",  RF_COUNT32, false },
    { 0xAB, "Program Fail Cnt",   RF_COUNT32, false },
    { 0xAC, "Erase Fail Count",   RF_COUNT32, false },
    { 0xAD, "Wear Level Count",   RF_COUNT32, false },
    { 0xAE, "Unexpect Poweroff",  RF_COUNT32, false },
    { 0xC2, "Temperature",        RF_TEMP,    false },
    { 0xF1, "Total Writes GiB",   RF_GIB,     false },
    { 0xF2, "Total Reads GiB",    RF_GIB,     false },
};

// --- Silicon Motion controller (ADATA SU/SX, Transcend, SPCC, Team, Gigabyte)
static const SmartAttrDef k_silicon_motion[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
    { 0xAA, "Available Reserve",  RF_COUNT32, false },
    { 0xAD, "Wear Level Count",   RF_COUNT32, false },
    { 0xAE, "Unexpect Poweroff",  RF_COUNT32, false },
    { 0xC2, "Temperature",        RF_TEMP,    false },
    { 0xF1, "Total Writes GiB",   RF_GIB,     false },
    { 0xF2, "Total Reads GiB",    RF_GIB,     false },
};

// --- Marvell controller (SanDisk Ultra/Plus/Extreme, WD Blue/Green SSD) -----
static const SmartAttrDef k_marvell_ssd[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
    { 0xAB, "Program Fail Cnt",   RF_COUNT32, false },
    { 0xAC, "Erase Fail Count",   RF_COUNT32, false },
    { 0xAD, "Wear Level Count",   RF_COUNT32, false },
    { 0xAE, "Unexpect Poweroff",  RF_COUNT32, false },
    { 0xB5, "Prgm Fail Cnt Tot",  RF_COUNT32, false },
    { 0xBB, "Uncorr ECC Count",   RF_COUNT32, true  },
    { 0xC2, "Temperature",        RF_TEMP,    false },
    { 0xC3, "ECC Error Rate",     RF_HEX,     false },
    { 0xE1, "Host Writes 32MiB",  RF_COUNT32, false },
    { 0xE8, "Available Reserve",  RF_PERCENT, false },
    { 0xE9, "Wear Indicator",     RF_COUNT32, false },
    { 0xF1, "Total Writes GiB",   RF_GIB,     false },
    { 0xF2, "Total Reads GiB",    RF_GIB,     false },
};

// --- SK Hynix SSD (SC / SL / SC308 / SC313) — own controller ----------------
static const SmartAttrDef k_hynix_ssd[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
    { 0xAA, "Available Reserve",  RF_COUNT32, false },
    { 0xAD, "Wear Level Count",   RF_COUNT32, false },
    { 0xB1, "NAND Writes (GiB)",  RF_GIB,     false },
    { 0xC2, "Temperature",        RF_TEMP,    false },
    { 0xCA, "Perc Lifetime Used", RF_PERCENT, false },
    { 0xF1, "Total Writes GiB",   RF_GIB,     false },
    { 0xF2, "Total Reads GiB",    RF_GIB,     false },
};

// --- Plextor M3/M5/M6/M7 (Marvell based) ------------------------------------
// Similar to k_marvell_ssd but with Plextor-specific attr names
static const SmartAttrDef k_plextor[] =
{
    { 0x09, "Power-On Hours",     RF_HOURS,   false },
    { 0xAD, "Wear Level Count",   RF_COUNT32, false },
    { 0xC2, "Temperature",        RF_TEMP,    false },
    { 0xE1, "Host Writes 32MiB",  RF_COUNT32, false },
    { 0xF1, "Total Writes GiB",   RF_GIB,     false },
    { 0xF2, "Total Reads GiB",    RF_GIB,     false },
};

// ============================================================================
// Vendor prefix table
// First match wins. More specific prefixes listed before general ones.
// Brand aliases for the same controller share the same table pointer.
// ============================================================================

static const SmartVendorEntry k_vendors[] =
{
    // ---- Spinning HDD -------------------------------------------------------

    // Seagate — all families use ST prefix
    { "ST",             k_seagate,      3  },

    // Western Digital HDD — no overrides, defaults are correct
    { "WDC ",           k_wd_hdd,       0  },
    { "WD ",            k_wd_hdd,       0  },

    // Samsung HDD SpinPoint — OEM Xbox era
    { "SAMSUNG SP",     k_samsung_hdd,  2  },
    { "SAMSUNG HD",     k_samsung_hdd,  2  },

    // IBM / early Hitachi Deskstar
    { "IBM-DTLA",       k_ibm,          2  },
    { "IC35",           k_ibm,          2  },
    { "IBM-",           k_ibm,          2  },

    // Hitachi / HGST
    { "HDS",            k_hitachi,      3  },
    { "HDT",            k_hitachi,      3  },
    { "HUA",            k_hitachi,      3  },
    { "HTE",            k_hitachi,      3  },
    { "HGST H",         k_hitachi,      3  },
    { "HGST",           k_hitachi,      3  },
    { "Hitachi",        k_hitachi,      3  },
    { "HITACHI",        k_hitachi,      3  },

    // Toshiba — MK (2.5" minutes) before generic TOSHIBA (hours)
    { "TOSHIBA MK",     k_toshiba_mk,   3  },
    { "TOSHIBA DT",     k_toshiba_35,   3  },
    { "TOSHIBA MD",     k_toshiba_35,   3  },
    { "TOSHIBA MG",     k_toshiba_35,   3  },
    { "TOSHIBA HD",     k_toshiba_35,   3  },
    { "TOSHIBA",        k_toshiba_35,   3  },  // safe fallback for unknowns
    { "Toshiba",        k_toshiba_35,   3  },

    // Fujitsu — original Xbox OEM drives
    { "MHT",            k_fujitsu,      3  },
    { "MHV",            k_fujitsu,      3  },
    { "MHS",            k_fujitsu,      3  },
    { "MHY",            k_fujitsu,      3  },
    { "MHZ",            k_fujitsu,      3  },
    { "FUJITSU",        k_fujitsu,      3  },
    { "Fujitsu",        k_fujitsu,      3  },

    // Maxtor — OEM Xbox era
    { "6Y",             k_maxtor,       3  },
    { "6L",             k_maxtor,       3  },
    { "7Y",             k_maxtor,       3  },
    { "7L",             k_maxtor,       3  },
    { "Maxtor",         k_maxtor,       3  },
    { "MAXTOR",         k_maxtor,       3  },
    { "STM",            k_maxtor,       3  },  // post-Seagate-acquisition Maxtor

    // Quantum (pre-Maxtor)
    { "QUANTUM",        k_quantum,      1  },
    { "Quantum",        k_quantum,      1  },
    { "FIREBALL",       k_quantum,      1  },

    // ---- SSD — Samsung (own controller) ------------------------------------
    { "SAMSUNG MZ",     k_samsung_ssd,  13 },
    { "Samsung SSD",    k_samsung_ssd,  13 },
    { "SAMSUNG SSD",    k_samsung_ssd,  13 },

    // ---- SSD — Intel (own controller) --------------------------------------
    { "INTEL SSD",      k_intel_ssd,    8  },
    { "SSDSA",          k_intel_ssd,    8  },
    { "SSDSC",          k_intel_ssd,    8  },
    { "SSDSD",          k_intel_ssd,    8  },
    { "SSDPE",          k_intel_ssd,    8  },

    // ---- SSD — Crucial / Micron (Marvell / Silicon Motion) -----------------
    { "CT",             k_crucial,      10 },
    { "CRUCIAL",        k_crucial,      10 },
    { "Crucial",        k_crucial,      10 },
    { "MTFD",           k_crucial,      10 },
    { "Micron",         k_crucial,      10 },
    { "MICRON",         k_crucial,      10 },

    // ---- SSD — SandForce controller family ---------------------------------
    // OCZ
    { "OCZ-VERTEX",     k_sandforce,    12 },
    { "OCZ-AGILITY",    k_sandforce,    12 },
    { "OCZ-SOLID",      k_sandforce,    12 },
    { "OCZ-REVODRIVE",  k_sandforce,    12 },
    { "OCZ",            k_sandforce,    12 },
    // Kingston HyperX / SSDNow (SandForce based models)
    { "KINGSTON SH",    k_sandforce,    12 },  // HyperX
    // Corsair Force / Nova
    { "Corsair Force",  k_sandforce,    12 },
    { "Corsair CSSD",   k_sandforce,    12 },
    { "CORSAIR",        k_sandforce,    12 },
    // ADATA SP / SX SandForce models
    { "ADATA SP3",      k_sandforce,    12 },
    { "ADATA SP5",      k_sandforce,    12 },
    { "ADATA SP6",      k_sandforce,    12 },
    { "ADATA SP8",      k_sandforce,    12 },
    { "ADATA SP9",      k_sandforce,    12 },
    { "ADATA SX9",      k_sandforce,    12 },
    // Mushkin Chronos / Enhanced
    { "MKNSSD",         k_sandforce,    12 },
    { "Mushkin",        k_sandforce,    12 },

    // ---- SSD — Phison controller family ------------------------------------
    // Kingston A400 / UV400 / UV500 (Phison PS3111 / PS3110)
    { "KINGSTON SA",    k_phison,       9  },  // A400
    { "KINGSTON SU",    k_phison,       9  },  // UV400/UV500
    { "KINGSTON RB",    k_phison,       9  },  // RBU series
    { "KINGSTON",       k_phison,       9  },  // general fallback
    // PNY CS series
    { "PNY CS",         k_phison,       9  },
    { "PNY",            k_phison,       9  },
    // Patriot Burst / Ignite
    { "Patriot",        k_phison,       9  },
    { "PATRIOT",        k_phison,       9  },
    // Team Group (T-Force / L5 Lite)
    { "T-FORCE",        k_phison,       9  },
    { "TEAML5",         k_phison,       9  },
    { "Team",           k_phison,       9  },

    // ---- SSD — Silicon Motion controller family ----------------------------
    // ADATA SU series (Silicon Motion SM2258 / SM2259)
    { "ADATA SU",       k_silicon_motion, 7 },
    { "ADATA",          k_silicon_motion, 7 },
    // Silicon Power / SPCC
    { "SPCC",           k_silicon_motion, 7 },
    { "Silicon Power",  k_silicon_motion, 7 },
    // Transcend SSD (JMicron / Silicon Motion)
    { "TS",             k_silicon_motion, 7 },
    // Gigabyte (Silicon Motion)
    { "GIGABYTE GP",    k_silicon_motion, 7 },
    // KingFast / KingSpec (JMicron / Silicon Motion)
    { "KingFast",       k_silicon_motion, 7 },
    { "KingSpec",       k_silicon_motion, 7 },
    { "KINGSPEC",       k_silicon_motion, 7 },
    // Intenso (Silicon Motion OEM)
    { "Intenso",        k_silicon_motion, 7 },
    { "INTENSO",        k_silicon_motion, 7 },

    // ---- SSD — Marvell controller family -----------------------------------
    // SanDisk (Marvell 88SS9xxx)
    { "SanDisk SSD",    k_marvell_ssd,  14 },
    { "SanDisk SD",     k_marvell_ssd,  14 },
    { "SanDisk",        k_marvell_ssd,  14 },
    { "SANDISK",        k_marvell_ssd,  14 },
    // WD SSD (acquired SanDisk, same Marvell base)
    { "WDC WDS",        k_marvell_ssd,  14 },
    { "WD Blue SSD",    k_marvell_ssd,  14 },
    { "WD Green SSD",   k_marvell_ssd,  14 },
    // Plextor
    { "PLEXTOR PX",     k_plextor,      6  },
    { "PLEXTOR",        k_plextor,      6  },

    // ---- SSD — SK Hynix ----------------------------------------------------
    { "HFS",            k_hynix_ssd,    8  },
    { "HFM",            k_hynix_ssd,    8  },
    { "SK Hynix",       k_hynix_ssd,    8  },
    { "SKHynix",        k_hynix_ssd,    8  },
};

static const int k_vendorCount =
(int)(sizeof(k_vendors) / sizeof(k_vendors[0]));

// ============================================================================
// SmartDB_Lookup
// ============================================================================

static inline const SmartAttrDef* SmartDB_Lookup(const char* model, BYTE id)
{
    int vi, ai, di;

    for (vi = 0; vi < k_vendorCount; ++vi)
    {
        const SmartVendorEntry& ve = k_vendors[vi];
        const char* m = model;
        const char* p = ve.prefix;
        bool match = true;
        while (*p)
        {
            if (*m != *p) { match = false; break; }
            ++m; ++p;
        }
        if (!match) continue;

        // Vendor prefix matched — search override table
        for (ai = 0; ai < ve.attrCount; ++ai)
        {
            if (ve.attrs[ai].id == id && ve.attrs[ai].name != NULL)
                return &ve.attrs[ai];
        }
        // Vendor matched but attr not overridden — fall through to defaults
        break;
    }

    for (di = 0; di < k_defaultAttrCount; ++di)
    {
        if (k_defaultAttrs[di].id == id)
            return &k_defaultAttrs[di];
    }

    return NULL;
}

// ============================================================================
// SmartDB_FormatRaw
// Decodes 6-byte SMART raw value per RawFmt.
// raw[0]=LSB, raw[5]=MSB (little-endian from ATA SMART buffer).
// outLen must be >= 24.
// ============================================================================

static inline void SmartDB_FormatRaw(RawFmt fmt, const BYTE* raw,
    char* out, int outLen)
{
    unsigned long val32;
    unsigned int  val16;
    unsigned long hours;
    unsigned long mins;
    char tmp[12];
    int  ri;

    val32 = (unsigned long)raw[0]
        | ((unsigned long)raw[1] << 8)
        | ((unsigned long)raw[2] << 16)
        | ((unsigned long)raw[3] << 24);

    val16 = (unsigned int)raw[0] | ((unsigned int)raw[1] << 8);

    switch (fmt)
    {
    case RF_COUNT32:
        IntToStr((int)val32, out, outLen);
        break;

    case RF_HOURS:
        // Matches smartmontools raw24(raw8): lower 24 bits are hours.
        // raw[3..5] are supplementary bytes used by some vendors for other data —
        // we deliberately exclude them to avoid inflated readings.
    {
        unsigned long hours24 = (unsigned long)raw[0]
            | ((unsigned long)raw[1] << 8)
            | ((unsigned long)raw[2] << 16);
        IntToStr((int)hours24, out, outLen);
        StrCat2(out, outLen, out, "h");
    }
    break;

    case RF_MINUTES:
        hours = val32 / 60UL;
        mins = val32 % 60UL;
        IntToStr((int)hours, out, outLen);
        StrCat2(out, outLen, out, "h ");
        IntToStr((int)mins, tmp, sizeof(tmp));
        StrCat2(out, outLen, out, tmp);
        StrCat2(out, outLen, out, "m");
        break;

    case RF_TEMP:
    {
        // Decode matches smartmontools tempminmax heuristic (atacmds.cpp check_temp_range).
        // raw[0] = current temp as signed byte.
        // raw[2]/raw[3] = min/max as signed bytes — only shown if they pass sanity:
        //   -60 <= min <= current <= max <= 120, and not the degenerate (-1, <=0) pair.
        int cur = (int)(signed char)raw[0];
        int mn = (int)(signed char)raw[2];
        int mx = (int)(signed char)raw[3];
        bool showRange;

        if (mn > mx) { int t = mn; mn = mx; mx = t; }  // swap if inverted

        showRange = (mn >= -60 && mn <= cur && cur <= mx && mx <= 120
            && !(mn == -1 && mx <= 0));

        IntToStr(cur, out, outLen);
        StrCat2(out, outLen, out, "C");
        if (showRange)
        {
            StrCat2(out, outLen, out, " (");
            IntToStr(mn, tmp, sizeof(tmp));
            StrCat2(out, outLen, out, tmp);
            StrCat2(out, outLen, out, "/");
            IntToStr(mx, tmp, sizeof(tmp));
            StrCat2(out, outLen, out, tmp);
            StrCat2(out, outLen, out, ")");
        }
        break;
    }

    case RF_SECTORS:
        IntToStr((int)val16, out, outLen);
        StrCat2(out, outLen, out, " sct");
        break;

    case RF_PERCENT:
        IntToStr((int)raw[0], out, outLen);
        StrCat2(out, outLen, out, "%");
        break;

    case RF_SECONDS:
        // sec2hour: raw32 seconds -> "Xh Ym Zs"
    {
        unsigned long secs = val32 % 60UL;
        unsigned long m = val32 / 60UL;
        unsigned long h = m / 60UL;
        m = m % 60UL;
        IntToStr((int)h, out, outLen);
        StrCat2(out, outLen, out, "h ");
        IntToStr((int)m, tmp, sizeof(tmp));
        StrCat2(out, outLen, out, tmp);
        StrCat2(out, outLen, out, "m ");
        IntToStr((int)secs, tmp, sizeof(tmp));
        StrCat2(out, outLen, out, tmp);
        StrCat2(out, outLen, out, "s");
    }
    break;

    case RF_RAW16_OPT16:
        // raw16(raw16): lower 16 bits as primary count.
        // Upper 16 bits shown in parens if nonzero (secondary counter on some drives).
        // Matches smartmontools default for attrs 0x05 and 0xC4.
    {
        unsigned int lo = val16;
        unsigned int hi = ((unsigned int)raw[2] | ((unsigned int)raw[3] << 8));
        IntToStr((int)lo, out, outLen);
        if (hi != 0)
        {
            StrCat2(out, outLen, out, " (");
            IntToStr((int)hi, tmp, sizeof(tmp));
            StrCat2(out, outLen, out, tmp);
            StrCat2(out, outLen, out, ")");
        }
    }
    break;

    case RF_RAW16_AVG16:
        // raw16(avg16): lower 16 bits in ms, upper 16 = average if nonzero.
        // Matches smartmontools default for attr 0x03 (Spin-Up Time).
    {
        unsigned int ms = val16;
        unsigned int avg = ((unsigned int)raw[2] | ((unsigned int)raw[3] << 8));
        IntToStr((int)ms, out, outLen);
        StrCat2(out, outLen, out, "ms");
        if (avg != 0)
        {
            StrCat2(out, outLen, out, " avg:");
            IntToStr((int)avg, tmp, sizeof(tmp));
            StrCat2(out, outLen, out, tmp);
            StrCat2(out, outLen, out, "ms");
        }
    }
    break;

    case RF_RAW16:
        IntToStr((int)val16, out, outLen);
        break;

    case RF_GIB:
        IntToStr((int)val32, out, outLen);
        StrCat2(out, outLen, out, " GiB");
        break;

    case RF_HEX:
    default:
        out[0] = '\0';
        for (ri = 5; ri >= 0; --ri)
        {
            char hb[4];
            IntToHex(raw[ri], 2, hb, sizeof(hb));
            StrCat2(out, outLen, out, hb);
            if (ri > 0) StrCat2(out, outLen, out, " ");
        }
        break;
    }
}