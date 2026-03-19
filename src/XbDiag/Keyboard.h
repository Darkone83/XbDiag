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

// Rendering mode.
//   KB_FULLSCREEN — takes over the entire frame (BeginScene/EndScene/Present
//                   are managed internally).  Default — existing callers need
//                   no changes.
//   KB_OVERLAY    — caller owns the scene brackets.  Caller does BeginScene,
//                   renders their content, then calls Keyboard_Tick which draws
//                   the keyboard panel into the already-open scene.  Caller
//                   then does EndScene/Present.
enum KeyboardMode { KB_FULLSCREEN = 0, KB_OVERLAY };

// Function row key indices — used by Keyboard_GetSelectedFnKey()
enum FnKey { FK_CAPS = 0, FK_SPACE, FK_BKSP, FK_CR, FK_DONE, FK_COUNT };

// Open the keyboard.  title is shown in the page chrome top bar (fullscreen)
// or in the overlay panel header (overlay).
// buf must be at least maxLen+1 bytes.  maxLen is the max typed string length.
// mode defaults to KB_FULLSCREEN — existing callers require no changes.
void Keyboard_Open(const char* title,
    char* buf,
    int              maxLen,
    KeyboardDoneFn   onDone,
    KeyboardCancelFn onCancel,
    KeyboardMode     mode = KB_FULLSCREEN);

// Returns true while the keyboard is open.
bool Keyboard_IsActive();

// Call every frame while Keyboard_IsActive().  Handles input + rendering.
void Keyboard_Tick(const DiagLogo& logo);

// ── FileViewer edit mode API ──────────────────────────────────────────────────
// These allow FileViewer to drive the keyboard overlay directly without going
// through Keyboard_Tick — input comes from FileViewer, rendering is separate.

// Render the overlay panel into the currently open scene (no scene brackets).
void Keyboard_RenderOverlay();

// Move the key selector one step in the given direction.
// dir = +1 or -1.  Wraps within the row/column bounds.
void Keyboard_StepCol(int dir);   // left/right within a row
void Keyboard_StepRow(int dir);   // up/down between rows

// Returns the character of the currently selected key (rows 0-4).
// Returns 0 if the selected key is a function key (row 5).
char Keyboard_GetSelectedChar();

// Returns the FK_* enum value if selection is on row 5, else -1.
int  Keyboard_GetSelectedFnKey();

// Activate the currently selected function key (row 5: CAPS/SPACE/BKSP/DONE).
// Has no effect if selection is on a typable key row.
void Keyboard_ActivateSelected();

// Toggle the caps/symbol layer.
void Keyboard_ToggleCaps();