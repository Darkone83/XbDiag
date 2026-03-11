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

// -----------------------------------------------------------------------------
// InitInput
// -----------------------------------------------------------------------------
void InitInput()
{
    XInitDevices(0, 0);
    memset(g_padHandles, 0, sizeof(g_padHandles));
    memset(g_padLastPacket, 0, sizeof(g_padLastPacket));
    memset(g_padStates, 0, sizeof(g_padStates));
    memset(g_padButtons, 0, sizeof(g_padButtons));
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
                    g_padLastPacket[i] = 0;
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
                g_padLastPacket[i] = 0;
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

                // Begin with native digital bits:
                //  D-Pad / Start / Back / thumb clicks
                WORD mask = st.Gamepad.wButtons;

                // Analog A/B/X/Y buttons -> convert to digital
                const BYTE* a = st.Gamepad.bAnalogButtons;

                if (a[XINPUT_GAMEPAD_A] > ANALOG_THRESHOLD)
                    mask |= BTN_A;
                if (a[XINPUT_GAMEPAD_B] > ANALOG_THRESHOLD)
                    mask |= BTN_B;
                if (a[XINPUT_GAMEPAD_X] > ANALOG_THRESHOLD)
                    mask |= BTN_X;
                if (a[XINPUT_GAMEPAD_Y] > ANALOG_THRESHOLD)
                    mask |= BTN_Y;
                if (a[XINPUT_GAMEPAD_BLACK] > ANALOG_THRESHOLD)
                    mask |= BTN_BLACK;
                if (a[XINPUT_GAMEPAD_WHITE] > ANALOG_THRESHOLD)
                    mask |= BTN_WHITE;
                if (a[XINPUT_GAMEPAD_LEFT_TRIGGER] > ANALOG_THRESHOLD)
                    mask |= BTN_LTRIG;
                if (a[XINPUT_GAMEPAD_RIGHT_TRIGGER] > ANALOG_THRESHOLD)
                    mask |= BTN_RTRIG;

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
// GetTriggers – returns raw 0..255 analog values for all analog buttons
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