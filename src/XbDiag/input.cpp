#include "input.h"
#include <string.h>

#define MAX_PORTS 4
#define ANALOG_THRESHOLD 30       // 0..255 analog-button threshold
#define STICK_DEADZONE  8000      // stick deadzone for GetSticks()

static HANDLE       g_padHandles[MAX_PORTS];
static DWORD        g_padLastPacket[MAX_PORTS];
static XINPUT_STATE g_padStates[MAX_PORTS];
static WORD         g_padButtons[MAX_PORTS];   // synthesized BTN_* mask
static WORD         s_rumbleLeft = 0;         // last sent rumble values
static WORD         s_rumbleRight = 0;
static DWORD        s_rumbleLastMs = 0;        // GetTickCount() of last XInputSetState call
static int          s_rumblePort = -1;        // port whose handle we call XInputSetState on

// Memory Unit presence — slot 0 = top (A), slot 1 = bottom (B) per port.
// Tracked via XGetDeviceChanges bit flags — no handles opened, no data read.
static bool g_muPresent[MAX_PORTS][2];

// -----------------------------------------------------------------------------
// InitInput
// -----------------------------------------------------------------------------
void InitInput()
{
    XInitDevices(0, 0);
    memset(g_padHandles, 0, sizeof(g_padHandles));
    memset(g_padLastPacket, 0xFF, sizeof(g_padLastPacket)); // 0xFFFFFFFF: ensures first real packet always processes
    memset(g_padStates, 0, sizeof(g_padStates));
    memset(g_padButtons, 0, sizeof(g_padButtons));
    memset(g_muPresent, 0, sizeof(g_muPresent));
    s_rumbleLeft = 0;  // motors start off — no need to send a stop packet
    s_rumbleRight = 0;
    s_rumblePort = -1;
}

// -----------------------------------------------------------------------------
// PumpInput  – reads controller state + synthesizes BTN_*
// -----------------------------------------------------------------------------
void PumpInput()
{
    DWORD ins = 0, rem = 0;

    // Hotplug handling
    if (XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &ins, &rem))
    {
        for (int i = 0; i < MAX_PORTS; ++i)
        {
            if (ins & 1)
            {
                if (!g_padHandles[i])
                {
                    g_padHandles[i] = XInputOpen(
                        XDEVICE_TYPE_GAMEPAD, i, XDEVICE_NO_SLOT, NULL);
                    g_padLastPacket[i] = 0xFFFFFFFF; // ensures first packet always processes
                }
            }
            if (rem & 1)
            {
                if (g_padHandles[i])
                {
                    XInputClose(g_padHandles[i]);
                    g_padHandles[i] = NULL;
                }
                // Zero state so GetSticks/GetTriggers don't return stale values
                ZeroMemory(&g_padStates[i], sizeof(g_padStates[i]));
                g_padButtons[i] = 0;
                g_padLastPacket[i] = 0xFFFFFFFF; // ensures first packet after reconnect is always processed
                // Reset rumble tracking so stop packet fires on reconnect
                s_rumbleLeft = 0xFFFF;
                s_rumbleRight = 0xFFFF;
                if (s_rumblePort == i)
                    s_rumblePort = -1;
            }

            ins >>= 1;
            rem >>= 1;
        }
    }

    // Memory Unit hotplug — presence only, no mounting.
    // Bit layout from XGetDeviceChanges for MEMORY_UNIT:
    //   bits  0-3:  slot A (top)    ports 0-3
    //   bits 16-19: slot B (bottom) ports 0-3
    if (XGetDeviceChanges(XDEVICE_TYPE_MEMORY_UNIT, &ins, &rem))
    {
        for (int i = 0; i < MAX_PORTS; ++i)
        {
            DWORD maskA = 1u << i;          // slot A bit
            DWORD maskB = 1u << (i + 16);   // slot B bit
            if (ins & maskA) g_muPresent[i][0] = true;
            if (rem & maskA) g_muPresent[i][0] = false;
            if (ins & maskB) g_muPresent[i][1] = true;
            if (rem & maskB) g_muPresent[i][1] = false;
        }
    }

    // Read pad states
    for (int i = 0; i < MAX_PORTS; ++i)
    {
        if (!g_padHandles[i])
        {
            g_padButtons[i] = 0;
            continue;
        }

        XINPUT_STATE st;
        ZeroMemory(&st, sizeof(st));

        if (XInputGetState(g_padHandles[i], &st) == ERROR_SUCCESS)
        {
            s_rumblePort = i;  // this handle is responsive — safe to rumble
            if (st.dwPacketNumber != g_padLastPacket[i])
            {
                g_padLastPacket[i] = st.dwPacketNumber;
                g_padStates[i] = st;

                // Standard digital bits — upper byte of wButtons is RESERVED on genuine
                // OG Xbox hardware and must be zero.  Third-party controllers (Retrofighter
                // and others) sometimes set upper wButtons bits for their own purposes, which
                // would collide exactly with the BTN_* synthetic constants (0x0100-0x8000).
                // Mask to the known-valid 8 bits only: d-pad, Start, Back, thumb clicks.
                WORD raw_w = st.Gamepad.wButtons;
                WORD mask = raw_w & 0x00FF;

                // Analog buttons — face buttons, triggers, Black/White.
                // Standard controllers (Duke, S) use bAnalogButtons[0-7], values 0-255.
                // Digital-only controllers (Retrofighter) report 0 or 255 with no analog
                // ramp.  The threshold of ANALOG_THRESHOLD handles both correctly.
                //
                // Belt-and-suspenders: also accept the corresponding wButtons high bits
                // (0x1000-0x8000 for A/B/X/Y, 0x0100-0x0800 for Black/White/triggers).
                // Some Retrofighter firmware versions put face buttons in wButtons rather
                // than — or in addition to — bAnalogButtons.  Checking both paths means
                // the controller works regardless of which reporting method it uses.
                // These bits are safe to accept here because the & 0x00FF mask above has
                // already excluded them from the base mask.
                const BYTE* a = st.Gamepad.bAnalogButtons;

                if (a[XINPUT_GAMEPAD_A] > ANALOG_THRESHOLD || (raw_w & 0x1000)) mask |= BTN_A;
                if (a[XINPUT_GAMEPAD_B] > ANALOG_THRESHOLD || (raw_w & 0x2000)) mask |= BTN_B;
                if (a[XINPUT_GAMEPAD_X] > ANALOG_THRESHOLD || (raw_w & 0x4000)) mask |= BTN_X;
                if (a[XINPUT_GAMEPAD_Y] > ANALOG_THRESHOLD || (raw_w & 0x8000)) mask |= BTN_Y;
                if (a[XINPUT_GAMEPAD_BLACK] > ANALOG_THRESHOLD || (raw_w & 0x0100)) mask |= BTN_BLACK;
                if (a[XINPUT_GAMEPAD_WHITE] > ANALOG_THRESHOLD || (raw_w & 0x0200)) mask |= BTN_WHITE;
                if (a[XINPUT_GAMEPAD_LEFT_TRIGGER] > ANALOG_THRESHOLD || (raw_w & 0x0400)) mask |= BTN_LTRIG;
                if (a[XINPUT_GAMEPAD_RIGHT_TRIGGER] > ANALOG_THRESHOLD || (raw_w & 0x0800)) mask |= BTN_RTRIG;

                g_padButtons[i] = mask;
            }
        }
        else
        {
            // Disconnect or read error — zero everything so callers don't
            // see stale stick/trigger values from the last good packet
            ZeroMemory(&g_padStates[i], sizeof(g_padStates[i]));
            g_padButtons[i] = 0;
        }
    }
}

// -----------------------------------------------------------------------------
// GetButtons – returns synthesized unified mask from first connected pad
// -----------------------------------------------------------------------------
WORD GetButtons()
{
    for (int i = 0; i < MAX_PORTS; ++i)
    {
        if (g_padHandles[i])
            return g_padButtons[i];
    }
    return 0;
}

// -----------------------------------------------------------------------------
// GetSticks – returns left/right analog sticks (with deadzones)
// -----------------------------------------------------------------------------
void GetSticks(int& lx, int& ly, int& rx, int& ry)
{
    lx = ly = rx = ry = 0;

    for (int i = 0; i < MAX_PORTS; ++i)
    {
        if (!g_padHandles[i])
            continue;

        const XINPUT_GAMEPAD& gp = g_padStates[i].Gamepad;

        lx = gp.sThumbLX;
        ly = gp.sThumbLY;
        rx = gp.sThumbRX;
        ry = gp.sThumbRY;

        // Deadzone filtering
        if (abs(lx) < STICK_DEADZONE) lx = 0;
        if (abs(ly) < STICK_DEADZONE) ly = 0;
        if (abs(rx) < STICK_DEADZONE) rx = 0;
        if (abs(ry) < STICK_DEADZONE) ry = 0;

        return; // first connected pad only
    }
}

// -----------------------------------------------------------------------------
// GetRawSticks – returns unfiltered stick values (no deadzone applied).
// Used by the drift test in ControllerTest so it can detect sub-deadzone drift.
// -----------------------------------------------------------------------------
void GetRawSticks(int& lx, int& ly, int& rx, int& ry)
{
    lx = ly = rx = ry = 0;

    for (int i = 0; i < MAX_PORTS; ++i)
    {
        if (!g_padHandles[i])
            continue;

        const XINPUT_GAMEPAD& gp = g_padStates[i].Gamepad;
        lx = gp.sThumbLX;
        ly = gp.sThumbLY;
        rx = gp.sThumbRX;
        ry = gp.sThumbRY;
        return; // first connected pad only
    }
}
// -----------------------------------------------------------------------------
// GetTriggers – returns raw 0..255 analog values for all analog buttons
// from the first connected pad. Includes triggers, Black/White, and face buttons.
// -----------------------------------------------------------------------------
void GetTriggers(int& lt, int& rt, int& black, int& white,
    int& btnA, int& btnB, int& btnX, int& btnY)
{
    lt = rt = black = white = btnA = btnB = btnX = btnY = 0;

    for (int i = 0; i < MAX_PORTS; ++i)
    {
        if (!g_padHandles[i])
            continue;

        const BYTE* a = g_padStates[i].Gamepad.bAnalogButtons;
        lt = a[XINPUT_GAMEPAD_LEFT_TRIGGER];
        rt = a[XINPUT_GAMEPAD_RIGHT_TRIGGER];
        black = a[XINPUT_GAMEPAD_BLACK];
        white = a[XINPUT_GAMEPAD_WHITE];
        btnA = a[XINPUT_GAMEPAD_A];
        btnB = a[XINPUT_GAMEPAD_B];
        btnX = a[XINPUT_GAMEPAD_X];
        btnY = a[XINPUT_GAMEPAD_Y];
        return;
    }
}

// -----------------------------------------------------------------------------
// IsPortConnected – returns true if a controller handle is open on this port
// -----------------------------------------------------------------------------
bool IsPortConnected(int port)
{
    if (port < 0 || port >= MAX_PORTS) return false;
    return g_padHandles[port] != NULL;
}

// -----------------------------------------------------------------------------
// IsMUPresent – returns true if a Memory Unit is present on port/slot.
//   Tracked via XGetDeviceChanges flags — no handle opened, no data read.
//   slot 0 = top slot (A), slot 1 = bottom slot (B)
// -----------------------------------------------------------------------------
bool IsMUPresent(int port, int slot)
{
    if (port < 0 || port >= MAX_PORTS) return false;
    if (slot < 0 || slot > 1) return false;
    return g_muPresent[port][slot];
}

// -----------------------------------------------------------------------------
// SetRumble – drives left (low-freq) and right (high-freq) motors
// Only calls XInputSetState on the one port that last returned a successful
// XInputGetState — avoids blocking on a stale/bad handle.  Two guards:
//   1. Value change guard — skip if nothing changed
//   2. 100ms cooldown  — skip if last send was too recent
// -----------------------------------------------------------------------------
void SetRumble(WORD left, WORD right)
{
    if (left == s_rumbleLeft && right == s_rumbleRight)
        return;

    if (s_rumblePort < 0 || !g_padHandles[s_rumblePort])
        return;  // no known-good handle — don't risk a blocking call

    DWORD now = GetTickCount();
    if (now - s_rumbleLastMs < 100)
        return;  // cooldown

    // hEvent = NULL: XInputSetState is synchronous. On OG Xbox hardware this
    // completes in ~1ms (one USB full-speed frame). With the 100ms cooldown
    // above, this is at most 10 calls/sec — negligible overhead.
    // DO NOT use CreateEvent/CloseHandle here: closing the event handle while
    // the USB IRP still references it causes a kernel use-after-free crash.
    static XINPUT_FEEDBACK fb;
    ZeroMemory(&fb, sizeof(fb));
    fb.Rumble.wLeftMotorSpeed = left;
    fb.Rumble.wRightMotorSpeed = right;
    XInputSetState(g_padHandles[s_rumblePort], &fb);

    s_rumbleLeft = left;
    s_rumbleRight = right;
    s_rumbleLastMs = now;
}