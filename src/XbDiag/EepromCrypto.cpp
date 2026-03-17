// EepromCrypto.cpp
// XbDiag - Xbox EEPROM SHA1-HMAC + RC4 crypto.
//
// Implements the Xbox custom two-pass HMAC-SHA1 key derivation and RC4
// stream cipher used to protect the EEPROM security section (0x14-0x2F).
//
// Reference: PrometheOS Decrypt()/EncryptAndCalculateCRC(),
//            XboxEepromEditor HmacSha1.cs / RC4.cs

#include "EepromCrypto.h"

// ============================================================================
// Internal SHA1
// ============================================================================

static DWORD EV_Rotl32(DWORD x, int n) { return (x << n) | (x >> (32 - n)); }

struct EVSha1 { DWORD H[5]; BYTE B[64]; DWORD bi; DWORD bitLen; };

static void EVSha1Block(EVSha1& s)
{
    static const DWORD k[4] = { 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6 };
    DWORD w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = ((DWORD)s.B[i * 4] << 24) | ((DWORD)s.B[i * 4 + 1] << 16) | ((DWORD)s.B[i * 4 + 2] << 8) | s.B[i * 4 + 3];
    for (int i = 16; i < 80; ++i)
        w[i] = EV_Rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    DWORD a = s.H[0], b = s.H[1], c = s.H[2], d = s.H[3], e = s.H[4];
    for (int i = 0; i < 80; ++i)
    {
        DWORD f;
        switch (i / 20) {
        case 0: f = (b & c) | ((~b) & d); break;
        case 1: f = b ^ c ^ d;          break;
        case 2: f = (b & c) | (b & d) | (c & d); break;
        default:f = b ^ c ^ d;          break;
        }
        DWORD t = EV_Rotl32(a, 5) + f + e + w[i] + k[i / 20]; e = d; d = c; c = EV_Rotl32(b, 30); b = a; a = t;
    }
    s.H[0] += a; s.H[1] += b; s.H[2] += c; s.H[3] += d; s.H[4] += e; s.bi = 0;
}
static void EVSha1Update(EVSha1& s, const BYTE* d, int len)
{
    for (int i = 0; i < len; ++i) { s.B[s.bi++] = d[i]; s.bitLen += 8; if (s.bi == 64)EVSha1Block(s); }
}
static void EVSha1Final(EVSha1& s, BYTE out[20])
{
    if (s.bi > 55) { s.B[s.bi++] = 0x80; while (s.bi < 64)s.B[s.bi++] = 0; EVSha1Block(s); while (s.bi < 56)s.B[s.bi++] = 0; }
    else { s.B[s.bi++] = 0x80; while (s.bi < 56)s.B[s.bi++] = 0; }
    s.B[56] = s.B[57] = s.B[58] = s.B[59] = 0;
    s.B[60] = (BYTE)(s.bitLen >> 24); s.B[61] = (BYTE)(s.bitLen >> 16); s.B[62] = (BYTE)(s.bitLen >> 8); s.B[63] = (BYTE)s.bitLen;
    EVSha1Block(s);
    for (int i = 0; i < 20; ++i)out[i] = (BYTE)(s.H[i >> 2] >> (8 * (3 - (i & 3))));
}

// ============================================================================
// Xbox HMAC-SHA1 (two-pass with version-specific IVs)
// ============================================================================

static void EVXboxHmac(int ver, const BYTE* data, int len, BYTE out[20])
{
    static const DWORD ivs[4][2][5] = {
        {{0x85F9E51A,0xE04613D2,0x6D86A50C,0x77C32E3C,0x4BD717A4},{0x5D7A9C6B,0xE1922BEB,0xB82CCDBC,0x3137AB34,0x486B52B3}},
        {{0x72127625,0x336472B9,0xBE609BEA,0xF55E226B,0x99958DAC},{0x76441D41,0x4DE82659,0x2E8EF85E,0xB256FACA,0xC4FE2DE8}},
        {{0x39B06E79,0xC9BD25E8,0xDBC6B498,0x40B4389D,0x86BBD7ED},{0x9B49BED3,0x84B430FC,0x6B8749CD,0xEBFE5FE5,0xD96E7393}},
        {{0x8058763A,0xF97D4E0E,0x865A9762,0x8A3D920D,0x08995B2C},{0x01075307,0xA2F1E037,0x1186EEEA,0x88DA9992,0x168A5609}},
    };
    EVSha1 s; BYTE mid[20];
    for (int j = 0; j < 5; ++j)s.H[j] = ivs[ver][0][j]; s.bi = 0; s.bitLen = 512;
    EVSha1Update(s, data, len); EVSha1Final(s, mid);
    for (int j = 0; j < 5; ++j)s.H[j] = ivs[ver][1][j]; s.bi = 0; s.bitLen = 512;
    EVSha1Update(s, mid, 20); EVSha1Final(s, out);
}

// ============================================================================
// RC4
// ============================================================================

struct EVRC4 { BYTE S[256]; int x, y; };
static void EVRC4Init(EVRC4& c, const BYTE* key, int klen)
{
    c.x = c.y = 0; for (int i = 0; i < 256; ++i)c.S[i] = (BYTE)i;
    int j = 0;
    for (int i = 0; i < 256; ++i) { j = (key[i % klen] + c.S[i] + j) & 0xFF; BYTE t = c.S[i]; c.S[i] = c.S[j]; c.S[j] = t; }
}
static void EVRC4Crypt(EVRC4& c, BYTE* data, int len)
{
    for (int i = 0; i < len; ++i) {
        c.x = (c.x + 1) & 0xFF; c.y = (c.S[c.x] + c.y) & 0xFF;
        BYTE t = c.S[c.x]; c.S[c.x] = c.S[c.y]; c.S[c.y] = t;
        data[i] ^= c.S[(c.S[c.x] + c.S[c.y]) & 0xFF];
    }
}

// ============================================================================
// Public API
// ============================================================================

int EepCrypto_Decrypt(BYTE* buf)
{
    BYTE orig[28];
    for (int i = 0; i < 28; ++i)orig[i] = buf[0x14 + i];
    for (int v = 0; v < 4; ++v)
    {
        BYTE kh[20]; EVXboxHmac(v, buf + 0x00, 20, kh);
        BYTE plain[28]; for (int i = 0; i < 28; ++i)plain[i] = orig[i];
        EVRC4 rc4; EVRC4Init(rc4, kh, 20); EVRC4Crypt(rc4, plain, 28);
        BYTE chk[20]; EVXboxHmac(v, plain, 28, chk);
        bool ok = true; for (int i = 0; i < 20; ++i)if (chk[i] != buf[i]) { ok = false; break; }
        if (ok) { for (int i = 0; i < 28; ++i)buf[0x14 + i] = plain[i]; return v; }
        for (int i = 0; i < 28; ++i)buf[0x14 + i] = orig[i];
    }
    return -1;
}

void EepCrypto_Encrypt(BYTE* buf, int ver)
{
    for (int i = 0; i < 20; ++i)buf[i] = 0;
    EVXboxHmac(ver, buf + 0x14, 28, buf + 0x00);
    BYTE kh[20]; EVXboxHmac(ver, buf + 0x00, 20, kh);
    EVRC4 rc4; EVRC4Init(rc4, kh, 20);
    EVRC4Crypt(rc4, buf + 0x14, 8);
    EVRC4Crypt(rc4, buf + 0x1C, 20);
}