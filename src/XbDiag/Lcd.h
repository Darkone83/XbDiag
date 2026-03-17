#pragma once
// lcd.h
// XbDiag - Physical LCD display driver
//
// Drives a US2066-compatible 20x4 character OLED at SMBus 0x78 (7-bit 0x3C).
// Two-phase init:
//   LCD_Begin()    — call at app startup. Detects display, runs init sequence,
//                    shows splash. Works with no SysInfo data yet.
//   LCD_SetData()  — call once after SysInfo has loaded its data.
//   LCD_Tick()     — call every frame from main loop, always.
//   LCD_OnExit()   — optional, clears display when leaving app cleanly.

#include <xtl.h>

struct LCDData
{
    const char* boardRev;
    const char* modchipName;
    const char* cpuSpeedMHz;
    const char* gpuSpeedMHz;
    const char* hddModel;
    const char* hddSizeGB;
    const char* hddUDMA;
    const char* ipAddr;
    const char* macAddr;
};

void LCD_Begin();
void LCD_SetData(const LCDData& data);
void LCD_Tick(WORD curButtons, WORD prevButtons);
void LCD_OnExit();
bool LCD_IsPresent();