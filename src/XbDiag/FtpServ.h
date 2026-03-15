#pragma once
// FtpServ.h — XbDiag FTP server public interface
//
// The FTP server is a bare-minimum passive-mode server polled once per frame
// from FileExplorer_Tick via FtpServ_Tick(). All sockets are non-blocking.
//
// Credentials : xbox / xbox
// Port        : 21 (control)  2024-2055 (passive data)
// Clients     : one at a time
// Commands    : USER PASS SYST TYPE PWD CWD CDUP LIST RETR STOR PASV QUIT
//               DELE MKD RMD RNFR RNTO SIZE NOOP FEAT

#include "DiagCommon.h"
#include <winsockx.h>

// ============================================================================
// Constants shared with FileExplorer
// ============================================================================

#define FTP_MAX_PATH    256
#define FTP_MAX_NAME    64

// FTP widget geometry (defined here so FtpServ_DrawWidget can use them)
#define WIDGET_W        220.f
#define WIDGET_X        (SW - LM - WIDGET_W)
#define WIDGET_PAD      6.f
#define WIDGET_LINE_H   14.f

// String helpers used internally by FtpServ.cpp
// (defined in FileExplorer.cpp — declared here so FtpServ.cpp can call them)
void FtpServ_AppendStr(char* out, int outLen, const char* src);
void FtpServ_TruncName(const char* src, char* dst, int maxChars, int dstLen);


// ============================================================================
// FTP state (read by FileExplorer for widget rendering)
// ============================================================================

enum FtpState { FTP_OFF = 0, FTP_LISTEN, FTP_CONNECTED, FTP_TRANSFER };
enum FtpXfer { XFER_NONE = 0, XFER_LIST, XFER_RETR, XFER_STOR };

struct FtpCtx
{
    FtpState state;

    SOCKET   listenSock;
    SOCKET   ctrlSock;
    SOCKET   dataListen;
    SOCKET   dataSock;
    WORD     dataPort;

    bool     authed;
    bool     gotUser;
    bool     atVirtualRoot;

    char     cwd[256];

    FtpXfer  xferType;
    char     xferName[64];
    char     xferPath[256];
    HANDLE   xferFile;
    DWORD    xferTotal;
    DWORD    xferDone;

    bool     gotRnfr;
    char     rnfrPath[256];

    char     recvBuf[1024];
    int      recvLen;

    char     sendBuf[2048];
    int      sendLen;
    int      sendOff;

    char     retrBuf[65536];
    int      retrBufLen;
    int      retrBufOff;

    bool     listPending;
    bool     listVirtualRoot;
    char     listDir[256];

    char     listBuf[65536];
    int      listBufLen;
    int      listBufOff;
};

extern FtpCtx g_ftp;   // owned by FtpServ.cpp, read by FileExplorer for widget

// ============================================================================
// Lifecycle
// ============================================================================

// FtpServ_Start / FtpServ_Stop — called by FileExplorer on [Start] toggle.
// ipStr and ipOK are the current network state from FileExplorer.
void FtpServ_Start(const char* ipStr, bool ipOK);
void FtpServ_Stop();

// FtpServ_Tick — call every frame from FileExplorer_Tick.
void FtpServ_Tick();

// FtpServ_DrawWidget — renders the FTP status overlay.
// Called from FileExplorer Render().
void FtpServ_DrawWidget();