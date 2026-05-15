// ============================================================================
// HttpFiles.h -- HTTP file manager for XbDiag web UI
// ============================================================================
// Provides a browser-based file manager through simple HTTP endpoints.
// The browser JS uses fetch() to call these routes -- no FTP involved.
//
// Routes:
//   GET  /list?path=/          -- directory listing as JSON
//   GET  /download?path=/f.txt -- file download (handled by existing route)
//   POST /delete?path=/f.txt   -- delete file or empty directory
//   POST /mkdir?path=/folder   -- create directory
//
// HttpRptSrv.cpp calls HttpFiles_BuildPage() for GET /files (the UI page)
// and HttpFiles_ServeList/Delete/Mkdir for the API routes.
// ============================================================================
#pragma once
#include <xtl.h>
#include <winsockx.h>

// Path helpers used by ServeClient upload streaming
bool HttpFiles_SafePath(const char* path, char* out, int outLen);
bool HttpFiles_GetParam(const char* src, const char* key, char* out, int outLen);

// Build the Files page into the HttpRptSrv buffer via PageStart/BA/PageEnd
void HttpFiles_BuildPage();

// API endpoints -- send JSON directly to client socket, no buffer needed
void HttpFiles_ServeList(SOCKET c, const char* query);
void HttpFiles_ServeDownload(SOCKET c, const char* query);
void HttpFiles_ServeDelete(SOCKET c, const char* query);
void HttpFiles_ServeMkdir(SOCKET c, const char* query);
void HttpFiles_ServeUpload(SOCKET c, const char* query, const char* body, int bodyLen);