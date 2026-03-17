#pragma once
// Update.h
// XbDiag - Software update checker and downloader.
//
// Connects to GitHub raw content at boot (if network link is available) to
// fetch XbDiag.ver from:
//   https://raw.githubusercontent.com/Darkone83/XbDiag/main/xbe/XbDiag.ver
//
// Version string format: "MAJOR.MINOR.PATCH" e.g. "1.0.2"
// Optional trailing " Beta" / " RC" suffix is stripped before comparison.
//
// If remote > local the app immediately transitions to STATE_UPDATE so the
// user lands on the update screen instead of the main menu.
//
// Download writes directly to D:\XbDiag.xbe (overwrites running binary).
// On completion the user is prompted to relaunch via XLaunchNewImage.
//
// Public API:
//   Update_OnEnter()          - called by main.cpp on STATE_UPDATE entry
//   Update_Tick()             - called every frame from main.cpp game loop
//   Update_GetLocalVersion()  - returns "1.0.2 Beta" (used by AboutScreen)
//   Update_StartBootCheck()   - called once at boot to kick off silent check
//   Update_IsCheckComplete()  - true once boot check has a result
//   Update_BootFoundUpdate()  - true if boot check found a newer version

#include "DiagCommon.h"

// Called once at startup to begin the silent background version check.
// Safe to call before any D3D frame is rendered.
void Update_StartBootCheck();

// Returns true once the boot-time check has finished (success or failure).
bool Update_IsCheckComplete();

// Returns true if the boot check completed and found a newer remote version.
bool Update_BootFoundUpdate();

// Module entry / tick (standard XbDiag module contract)
void Update_OnEnter();
void Update_Tick(const DiagLogo& logo);

// Returns the local version string, e.g. "1.0.2 Beta".
// Used by AboutScreen so the version lives in exactly one place.
const char* Update_GetLocalVersion();