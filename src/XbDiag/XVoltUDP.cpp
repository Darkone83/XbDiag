// ============================================================================
// XVoltUdp.cpp — X-Volt power rail UDP client for XbDiag
// ============================================================================
//
// Two-phase discovery state machine:
//
//   XVOLT_IDLE       — not started
//   XVOLT_LISTENING  — socket open on port 4093, waiting for beacon
//   XVOLT_DISCOVERED — beacon received; 4093 closed, 4092 open for data
//
// If data on 4092 goes stale (> XVOLT_STALE_MS), 4092 is closed and the
// engine resets to XVOLT_LISTENING on 4093 to rediscover.
// ============================================================================

#include "XVoltUdp.h"
#include "DiagCommon.h"

#include <xtl.h>
#include <winsockx.h>

// ── Internal state ────────────────────────────────────────────────────────────

enum XVoltState { XVOLT_IDLE = 0, XVOLT_LISTENING, XVOLT_DISCOVERED };

static XVoltState  s_state = XVOLT_IDLE;
static SOCKET      s_sockBeacon = INVALID_SOCKET;   // 4093 — discovery only
static SOCKET      s_sockData = INVALID_SOCKET;   // 4092 — data only
static bool        s_discovered = false;
static DWORD       s_lastPacketMs = 0;

static XVoltReading s_reading;

// ── XOR checksum ─────────────────────────────────────────────────────────────
static BYTE CalcChecksum(const BYTE* data, int len)
{
    BYTE x = 0;
    int i;
    for (i = 0; i < len - 1; ++i) x ^= data[i];
    return x;
}

// ── IP → dotted-decimal string ────────────────────────────────────────────────
static void IpToStr(DWORD ip_nbo, char* buf, int len)
{
    BYTE a = (BYTE)(ip_nbo & 0xFF);
    BYTE b = (BYTE)((ip_nbo >> 8) & 0xFF);
    BYTE c = (BYTE)((ip_nbo >> 16) & 0xFF);
    BYTE d = (BYTE)((ip_nbo >> 24) & 0xFF);
    char tmp[6];
    buf[0] = '\0';
    IntToStr((int)a, tmp, sizeof(tmp)); StrCat2(buf, len, buf, tmp); StrCat2(buf, len, buf, ".");
    IntToStr((int)b, tmp, sizeof(tmp)); StrCat2(buf, len, buf, tmp); StrCat2(buf, len, buf, ".");
    IntToStr((int)c, tmp, sizeof(tmp)); StrCat2(buf, len, buf, tmp); StrCat2(buf, len, buf, ".");
    IntToStr((int)d, tmp, sizeof(tmp)); StrCat2(buf, len, buf, tmp);
}

// ── Open a single non-blocking UDP socket — matches net.cpp InitSocket() ─────
static SOCKET OpenUdpSocket(WORD port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char*)&yes, sizeof(yes));

    sockaddr_in sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);

    if (bind(s, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }

    return s;
}

static void CloseBeacon()
{
    if (s_sockBeacon != INVALID_SOCKET) { closesocket(s_sockBeacon); s_sockBeacon = INVALID_SOCKET; }
}

static void CloseData()
{
    if (s_sockData != INVALID_SOCKET) { closesocket(s_sockData); s_sockData = INVALID_SOCKET; }
}

static DWORD s_lastDiscover = 0;  // last discover broadcast timestamp

// ── Resend discover broadcast — called periodically during LISTENING ──────────
static void SendDiscover()
{
    sockaddr_in to;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(XVOLT_BEACON_PORT);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const char* discover = "XVDISC";
    sendto(s_sockBeacon, discover, 6, 0, (sockaddr*)&to, sizeof(to));
    s_lastDiscover = GetTickCount();
}
static void EnterListening()
{
    CloseData();
    s_sockBeacon = OpenUdpSocket(XVOLT_BEACON_PORT);
    if (s_sockBeacon == INVALID_SOCKET) { s_state = XVOLT_IDLE; return; }

    // Send a discover broadcast so X-Volt firmware can record our IP and
    // switch from broadcasting to unicasting directly to us.
    // X-Volt listens on 4093 — any packet from us is enough to trigger it.
    {
        sockaddr_in to;
        ZeroMemory(&to, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(XVOLT_BEACON_PORT);
        to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        const char* discover = "XVDISC";
        sendto(s_sockBeacon, discover, 6, 0, (sockaddr*)&to, sizeof(to));
    }

    s_state = XVOLT_LISTENING;
}

// ── Enter data phase — close 4093, open 4092 ─────────────────────────────────
static void EnterDiscovered()
{
    CloseBeacon();
    s_sockData = OpenUdpSocket(XVOLT_DATA_PORT);
    s_lastPacketMs = GetTickCount();
    s_state = (s_sockData != INVALID_SOCKET) ? XVOLT_DISCOVERED : XVOLT_LISTENING;
    if (s_state == XVOLT_LISTENING)
        s_sockBeacon = OpenUdpSocket(XVOLT_BEACON_PORT);   // fallback if data open failed
}

// ── Receive one datagram; returns bytes or -1 ─────────────────────────────────
static int Recv(SOCKET s, BYTE* buf, int bufLen, sockaddr_in* from)
{
    if (s == INVALID_SOCKET) return -1;
    int fromLen = sizeof(sockaddr_in);
    int n = recvfrom(s, (char*)buf, bufLen, 0, (sockaddr*)from, &fromLen);
    return (n == SOCKET_ERROR) ? -1 : n;
}

// ── Handle a beacon packet ────────────────────────────────────────────────────
static void HandleBeacon(const BYTE* buf, int len, const sockaddr_in& from)
{
    if (len < (int)sizeof(XVoltBeacon)) return;
    const XVoltBeacon* pkt = (const XVoltBeacon*)buf;
    if (pkt->magic != XVOLT_MAGIC)        return;
    if (pkt->type != XVOLT_TYPE_BEACON)  return;

    IpToStr(from.sin_addr.s_addr, s_reading.sourceIP, sizeof(s_reading.sourceIP));
    s_discovered = true;

    // Beacon confirmed — shift to data phase
    EnterDiscovered();
}

// ── Handle a data packet ──────────────────────────────────────────────────────
static void HandleData(const BYTE* buf, int len)
{
    if (len < (int)sizeof(XVoltPacket)) return;
    const XVoltPacket* pkt = (const XVoltPacket*)buf;
    if (pkt->magic != XVOLT_MAGIC)       return;
    if (pkt->type != XVOLT_TYPE_DATA)   return;
    if (pkt->checksum != CalcChecksum(buf, sizeof(XVoltPacket))) return;

    s_reading.v12 = pkt->v12;
    s_reading.i12 = pkt->i12;
    s_reading.v5 = pkt->v5;
    s_reading.i5 = pkt->i5;
    s_reading.v33 = pkt->v33;
    s_reading.i33 = pkt->i33;
    s_reading.railFault = (pkt->flags & XVOLT_FLAG_RAIL_FAULT) != 0;
    s_reading.noSensor = (pkt->flags & XVOLT_FLAG_NO_SENSOR) != 0;
    s_lastPacketMs = GetTickCount();
}

// ── Public API ────────────────────────────────────────────────────────────────

void XVoltUdp_Start()
{
    if (s_state != XVOLT_IDLE) return;

    // Network must be confirmed up — XNetGetTitleXnAddr returns a valid address
    // after the Update boot-check polling loop completes in main.cpp.
    XNADDR xna;
    ZeroMemory(&xna, sizeof(xna));
    DWORD st = XNetGetTitleXnAddr(&xna);
    if (st == XNET_GET_XNADDR_PENDING || (st & XNET_GET_XNADDR_NONE) || xna.ina.s_addr == 0)
        return;

    ZeroMemory(&s_reading, sizeof(s_reading));
    s_discovered = false;

    // Start in discovery phase
    EnterListening();
}

void XVoltUdp_Poll()
{
    BYTE        buf[64];
    sockaddr_in from;
    int         n;

    switch (s_state)
    {
    case XVOLT_IDLE:
        return;

    case XVOLT_LISTENING:
        // Resend discover every 2s so X-Volt can latch our IP
        if (GetTickCount() - s_lastDiscover >= 2000)
            SendDiscover();
        // Drain beacon socket — on first valid beacon EnterDiscovered() fires
        while ((n = Recv(s_sockBeacon, buf, sizeof(buf), &from)) > 0)
            HandleBeacon(buf, n, from);
        break;

    case XVOLT_DISCOVERED:
        // Drain data socket
        while ((n = Recv(s_sockData, buf, sizeof(buf), &from)) > 0)
            HandleData(buf, n);

        // Check for stale data — device dropped off, reset to discovery.
        // IsDiscovered() also returns false at this point so SysInfo clears.
        if (GetTickCount() - s_lastPacketMs > XVOLT_STALE_MS)
            EnterListening();
        break;
    }
}

void XVoltUdp_Stop()
{
    CloseBeacon();
    CloseData();
    s_state = XVOLT_IDLE;
    s_discovered = false;
}

bool XVoltUdp_GetData(XVoltReading& out)
{
    if (!s_discovered)        return false;
    if (s_lastPacketMs == 0)  return false;
    if (GetTickCount() - s_lastPacketMs > XVOLT_STALE_MS) return false;
    out = s_reading;
    return true;
}

bool XVoltUdp_IsDiscovered()
{
    // Returns false when stale so display layers clear automatically.
    // Engine will have already reset to LISTENING at this point.
    if (!s_discovered)       return false;
    if (s_lastPacketMs == 0) return false;
    if (GetTickCount() - s_lastPacketMs > XVOLT_STALE_MS) return false;
    return true;
}

bool XVoltUdp_WasSeen()
{
    // True if a beacon was ever received this session regardless of stale state.
    // Use to distinguish "never detected" from "was connected, now disconnected".
    return s_discovered;
}