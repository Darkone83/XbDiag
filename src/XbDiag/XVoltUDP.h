// ============================================================================
// XVoltUdp.h — X-Volt power rail UDP client
// ============================================================================
//
// Listens for X-Volt broadcasts and maintains the most recent rail readings.
//
// Protocol (matches X-Volt firmware udp_xbdiag.h):
//   PORT 4093  BEACON  — XVoltBeacon every 2s  (device discovery)
//   PORT 4092  DATA    — XVoltPacket every 500ms (live rail readings)
//
// Usage:
//   XVoltUdp_Start()   — call once after XNetStartup / WSAStartup are up
//                         (safe to call before network is ready; defers bind)
//   XVoltUdp_Poll()    — call every frame from the main loop
//   XVoltUdp_GetData() — fills XVoltReading; returns true if data is fresh
//   XVoltUdp_Stop()    — close sockets (call on shutdown or net teardown)
//
// Data is considered stale after XVOLT_STALE_MS with no packet received.
// GetData returns false when stale so callers can show N/A rather than
// a frozen value.
// ============================================================================

#pragma once
#include <xtl.h>
#include <winsockx.h>

// ── Wire constants (must match X-Volt firmware) ───────────────────────────────
#define XVOLT_BEACON_PORT       4093
#define XVOLT_DATA_PORT         4092
#define XVOLT_MAGIC             0x584F4C56UL   // "XVOL"
#define XVOLT_TYPE_BEACON       0x01
#define XVOLT_TYPE_DATA         0x02

#define XVOLT_FLAG_DATA_VALID   0x01
#define XVOLT_FLAG_RAIL_FAULT   0x02
#define XVOLT_FLAG_NO_SENSOR    0x04

// Data is considered stale if no packet arrives within this window.
// X-Volt sends every 500ms — 5000ms gives 10 missed packets before clearing.
// When stale: engine resets to LISTENING, IsDiscovered() returns false,
// SysInfo inline section disappears automatically — clean silent disconnect.
#define XVOLT_STALE_MS          5000

// ── Wire packet layouts (must match X-Volt firmware exactly) ─────────────────
#pragma pack(push, 1)

struct XVoltBeacon
{
    DWORD    magic;        // XVOLT_MAGIC
    BYTE     type;         // XVOLT_TYPE_BEACON
    BYTE     proto;        // protocol version = 1
    WORD     data_port;    // data port (4092)
    WORD     fw_version;   // firmware BCD
    char     name[6];      // "X-Volt", null-padded
};

struct XVoltPacket
{
    DWORD    magic;        // XVOLT_MAGIC
    BYTE     type;         // XVOLT_TYPE_DATA
    BYTE     version;      // protocol version = 1
    BYTE     flags;        // XVOLT_FLAG_* bitmask
    float    v12;          // 12V rail voltage (V)
    float    i12;          // 12V rail current (mA)
    float    v5;           // 5V  rail voltage (V)
    float    i5;           // 5V  rail current (mA)
    float    v33;          // 3.3V rail voltage (V)
    float    i33;          // 3.3V rail current (mA)
    BYTE     checksum;     // XOR of all preceding bytes
};

#pragma pack(pop)

// ── Public reading struct — what callers see ──────────────────────────────────
struct XVoltReading
{
    float v12;             // 12V rail voltage (V)
    float i12;             // 12V rail current (mA)
    float v5;              // 5V  rail voltage (V)
    float i5;              // 5V  rail current (mA)
    float v33;             // 3.3V rail voltage (V)
    float i33;             // 3.3V rail current (mA)
    bool  railFault;       // any rail outside expected band
    bool  noSensor;        // INA3221 not detected on X-Volt
    char  sourceIP[20];    // dotted-decimal IP of the X-Volt device
};

// ── Public API ────────────────────────────────────────────────────────────────
void XVoltUdp_Start();
void XVoltUdp_Poll();
void XVoltUdp_Stop();

// Returns true if a valid packet has been received and data is fresh.
// Fills `out` with the most recent readings.
// Returns false when no device has been seen or data is stale (> XVOLT_STALE_MS).
bool XVoltUdp_GetData(XVoltReading& out);

// Returns true if data is fresh and device is currently active.
// Returns false when stale or never seen — display layer clears automatically.
bool XVoltUdp_IsDiscovered();

// Returns true if a beacon was ever received this session, regardless of
// current stale state. Use to show "X-Volt disconnected" vs "never seen".
bool XVoltUdp_WasSeen();