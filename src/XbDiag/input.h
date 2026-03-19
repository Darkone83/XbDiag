#pragma once
#include <xtl.h>

// -----------------------------------------------------------------------------
// Unified digital mask used everywhere.
//
// Notes:
// - D-Pad, START/BACK, thumb clicks = native XINPUT bits.
// - ABXY = high-bit synthetic flags derived from analog buttons.
// -----------------------------------------------------------------------------

enum
{
    BTN_DPAD_UP = XINPUT_GAMEPAD_DPAD_UP,
    BTN_DPAD_DOWN = XINPUT_GAMEPAD_DPAD_DOWN,
    BTN_DPAD_LEFT = XINPUT_GAMEPAD_DPAD_LEFT,
    BTN_DPAD_RIGHT = XINPUT_GAMEPAD_DPAD_RIGHT,

    BTN_START = XINPUT_GAMEPAD_START,
    BTN_BACK = XINPUT_GAMEPAD_BACK,
    BTN_LTHUMB = XINPUT_GAMEPAD_LEFT_THUMB,
    BTN_RTHUMB = XINPUT_GAMEPAD_RIGHT_THUMB,

    // Synthetic analog-button digital flags
    BTN_A = 0x1000,
    BTN_B = 0x2000,
    BTN_X = 0x4000,
    BTN_Y = 0x8000,
    BTN_BLACK = 0x0100,
    BTN_WHITE = 0x0200,
    BTN_LTRIG = 0x0400,
    BTN_RTRIG = 0x0800,
};

// -----------------------------------------------------------------------------
// API
// -----------------------------------------------------------------------------

// Initialize controller ports.
void InitInput();

// Polls & updates button state + analog stick state.
void PumpInput();

// Returns OR of all controller button masks (BTN_* flags above).
WORD GetButtons();

// Returns left/right stick raw 16-bit values.
// If no controller present: all zeros.
//   lx,ly = left stick   (-32768 .. 32767)
//   rx,ry = right stick  (-32768 .. 32767)
void GetSticks(int& lx, int& ly, int& rx, int& ry);
void GetRawSticks(int& lx, int& ly, int& rx, int& ry);  // no deadzone — for drift test

// Returns raw analog button values (0..255) for triggers, black/white, and face buttons.
void GetTriggers(int& lt, int& rt, int& black, int& white,
    int& btnA, int& btnB, int& btnX, int& btnY);

// Sets rumble motors on the first connected pad.
//   left  = left  (low-frequency) motor  0..65535
//   right = right (high-frequency) motor 0..65535
//   Pass 0,0 to stop rumble.
void SetRumble(WORD left, WORD right);

// Returns true if a controller is connected on this port (0-3).
bool IsPortConnected(int port);

// Returns true if a Memory Unit is present in the given port/slot.
//   port 0-3,  slot 0 = top (A),  slot 1 = bottom (B)
bool IsMUPresent(int port, int slot);

// -----------------------------------------------------------------------------
// Controller model — detected at connect time via XInputGetCapabilities SubType
// -----------------------------------------------------------------------------
enum ControllerType
{
    CT_UNKNOWN = 0,  // port empty or SubType unrecognised
    CT_DUKE,         // Original "Duke" controller  (SubType 0x01)
    CT_TYPE_S,       // Smaller "Type-S" controller (SubType 0x02)
};

// Returns detected model for port 0-3. CT_UNKNOWN if empty or unrecognised.
ControllerType GetControllerType(int port);

// -----------------------------------------------------------------------------
// Active test port — which connected port GetButtons/GetSticks/GetTriggers read.
// Defaults to the first connected port. ControllerTest cycles this with LT+Dpad.
// -----------------------------------------------------------------------------

// Returns the currently active port (0-3), or -1 if no controller connected.
int  GetActivePort();

// Sets the active port explicitly. No-op if port has no controller.
void SetActivePort(int port);

// Advances active port to next/prev connected port (wraps). No-op if <2 ports.
void StepActivePort(int dir);  // dir: +1 = next, -1 = prev