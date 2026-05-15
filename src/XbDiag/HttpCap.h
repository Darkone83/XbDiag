#pragma once
// HttpCap.h
// XbDiag - Framebuffer capture for HTTP live view
//
// Architecture:
//   HttpCap_Init()          -- call once at startup
//   HttpCap_CaptureFrame()  -- call from main loop between EndScene and Present
//   HttpCap_ServeScreenshot(c) -- serve BMP snapshot to browser socket
//   HttpCap_ServeLivePage(c, speed) -- serve auto-refresh HTML page

#include <xtl.h>

// Minimum ms between captures -- 2 seconds regardless of browser poll rate
#define HTTPCAP_MIN_INTERVAL_MS  10000

void HttpCap_Init();
void HttpCap_RequestFrame();
void HttpCap_CaptureFrame();
void HttpCap_ServeScreenshot(SOCKET c);
void HttpCap_ServeLivePage(SOCKET c, const char* live);
bool HttpCap_IsReady();