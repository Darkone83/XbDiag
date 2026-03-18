#pragma once
// Keyboard.h
// XbDiag virtual on-screen keyboard.
//
// Usage:
//   1. Call Keyboard_Open() with a title, a caller-owned buffer, max length,
//      and two callbacks — onDone (string confirmed) and onCancel (dismissed).
//   2. Each frame, if Keyboard_IsActive(), call Keyboard_Tick().
//      Tick handles both input and rendering — no separate Render call needed.
//   3. On confirm: onDone is called with a pointer to buf (already populated).
//      On cancel:  onCancel is called. buf contents are undefined.
//      After either callback Keyboard_IsActive() returns false.
//
// Controls:
//   D-Pad        — navigate keys
//   A            — type selected key
//   B            — cancel (no save)
//   X            — backspace
//   Y            — space (shortcut)
//   L3 (LTHUMB)  — toggle caps / symbol layer
//   START        — confirm / done
//
// Layout (6 rows):
//   Row 0:  1 2 3 4 5 6 7 8 9 0   (caps: ! @ # $ % - + = ( ))
//   Row 1:  q w e r t y u i o p   (caps: Q W E R T Y U I O P)
//   Row 2:  a s d f g h j k l     (caps: A S D F G H J K L  )
//   Row 3:  z x c v b n m         (caps: Z X C V B N M      )
//   Row 4:  . - _ @ / \ , ;       (same regardless of caps   )
//   Row 5:  [CAPS]  [SPACE]  [BKSP]  [DONE]
//
// Caller owns the buffer — Keyboard writes into it directly.
// buf must remain valid until the callback fires.

#include "DiagCommon.h"

// Callback types
typedef void (*KeyboardDoneFn)(const char* str);
typedef void (*KeyboardCancelFn)();

// Open the keyboard.  title is shown in the page chrome top bar.
// buf must be at least maxLen+1 bytes.  maxLen is the max typed string length.
void Keyboard_Open(const char* title,
    char* buf,
    int              maxLen,
    KeyboardDoneFn   onDone,
    KeyboardCancelFn onCancel);

// Returns true while the keyboard is open.
bool Keyboard_IsActive();

// Call every frame while Keyboard_IsActive().  Handles input + rendering.
void Keyboard_Tick(const DiagLogo& logo);