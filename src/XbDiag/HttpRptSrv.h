#pragma once
// HttpRptSrv.h
// XbDiag - HTTP Report Server
//
// Background service. No UI, no menu entry. Hooks into main.cpp
// identically to FtpServ:
//
//   HttpRptSrv_Start() -- once, after Update_StartBootCheck()
//   HttpRptSrv_Poll()  -- every frame at the bottom of the main loop
//
// Serves on port 80:
//   /          -> redirect to /sysinfo
//   /sysinfo   -> hardware snapshot from SysInfo
//   /report    -> report file viewer (XbDiag.txt and other exports)
//   /files     -> index of all XbDiag export files on D:\

void HttpRptSrv_Start();
void HttpRptSrv_Poll();