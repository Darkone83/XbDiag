// xba.cpp
// XbDiag - XBA archive extractor.  Supports v1 (XBA\x01) and v2 (XBA\x02).
//
// Output pattern mirrors zip.cpp exactly:
//   Out_Byte() writes one byte at a time directly to the file handle.
//   No large block assembly buffers -- eliminates the crash on Xbox FATX.
//
// Constraints:
//   No CRT (no sprintf/strlen/malloc).
//   No dynamic allocation -- static buffers only.
//   MSVC 2003 / C89 declaration ordering (all locals at top of scope).

#include "xba.h"
#include "DiagCommon.h"
#include <xtl.h>

// ============================================================================
// Constants
// ============================================================================

#define LZ_WIN_SIZE     16384
#define LZ_WIN_MASK     (LZ_WIN_SIZE - 1)
#define LZ_MIN_MATCH    3

#define LS_WIN_SIZE     32768
#define LS_WIN_MASK     (LS_WIN_SIZE - 1)
#define LS_MIN_MATCH    2

#define V1_FLAG_DIR         0x01
#define V1_FLAG_LZ77        0x00
#define V1_FLAG_X86_LZ77    0x02
#define V1_FLAG_LZSS        0x03
#define V1_FLAG_X86_LZSS    0x04

#define V2_FLAG_DIR         0x01
#define V2_FLAG_FILE        0x00
#define V2_FLAG_X86         0x02

#define V2_BLK_STORED       0x00
#define V2_BLK_LZ77         0x01
#define V2_BLK_LZSS         0x02
#define V2_BLK_RLE          0x03
#define V2_BLK_LZ77_HUFF    0x04
#define V2_BLK_LZSS_HUFF    0x05

#define HUFF_SYMS           256
#define HUFF_MAX_BITS       15
#define HUFF_TABLE_SIZE     (1 << HUFF_MAX_BITS)

static const BYTE k_magic_v1[4] = { 'X', 'B', 'A', 0x01 };
static const BYTE k_magic_v2[4] = { 'X', 'B', 'A', 0x02 };

// ============================================================================
// Detail buffer
// ============================================================================

static char s_detail[128];

static void SetDetail(const char* msg)
{
    int i = 0;
    while (msg[i] && i < 127) { s_detail[i] = msg[i]; ++i; }
    s_detail[i] = '\0';
}

static void SetDetailN(const char* prefix, int n)
{
    int  pi = 0;
    char nb[12];
    int  ni = 0;
    int  tmp;
    int  k;
    while (prefix[pi] && pi < 80) { s_detail[pi] = prefix[pi]; ++pi; }
    tmp = (n < 0) ? -n : n;
    if (tmp == 0) { nb[ni++] = '0'; }
    else { while (tmp > 0 && ni < 11) { nb[ni++] = '0' + (tmp % 10); tmp /= 10; } }
    if (n < 0 && pi < 126) s_detail[pi++] = '-';
    for (k = ni - 1; k >= 0 && pi < 127; --k) s_detail[pi++] = nb[k];
    s_detail[pi] = '\0';
}

static void CopyDetail(char* dst, int dstLen)
{
    int i = 0;
    while (s_detail[i] && i < dstLen - 1) { dst[i] = s_detail[i]; ++i; }
    dst[i] = '\0';
}

// ============================================================================
// CRC-32
// ============================================================================

static DWORD s_crcTab[256];
static int   s_crcOK = 0;

static void CRC_Init()
{
    DWORD i;
    if (s_crcOK) return;
    for (i = 0; i < 256; ++i)
    {
        DWORD c = i;
        int   j;
        for (j = 0; j < 8; ++j)
            c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        s_crcTab[i] = c;
    }
    s_crcOK = 1;
}

// ============================================================================
// Output state -- mirrors zip.cpp Out_* pattern exactly
// One byte at a time, direct WriteFile, no large assembly buffer
// ============================================================================

static HANDLE s_oh = INVALID_HANDLE_VALUE;
static int    s_oErr = 0;
static DWORD  s_oCrc = 0;
static DWORD  s_oCount = 0;

// 4KB write buffer -- safe for FATX MU cluster boundaries
#define OUT_BUF_SIZE 4096
static BYTE  s_outBuf[OUT_BUF_SIZE];
static DWORD s_outBufPos = 0;

// LZ windows -- used as output ring buffer for back-references
static BYTE s_lzWin[LZ_WIN_SIZE];
static DWORD s_lzPos = 0;

static BYTE s_lsWin[LS_WIN_SIZE];
static DWORD s_lsPos = 0;

static void Out_Flush()
{
    DWORD nw = 0;
    if (s_oErr || s_outBufPos == 0) return;
    if (!WriteFile(s_oh, s_outBuf, s_outBufPos, &nw, NULL) || nw != s_outBufPos)
        s_oErr = 1;
    s_outBufPos = 0;
}

static void Out_Init(HANDLE hf)
{
    s_oh = hf;
    s_oErr = 0;
    s_oCrc = 0xFFFFFFFFUL;
    s_oCount = 0;
    s_outBufPos = 0;
    s_lzPos = 0;
    s_lsPos = 0;
    ZeroMemory(s_lzWin, sizeof(s_lzWin));
    ZeroMemory(s_lsWin, sizeof(s_lsWin));
}

static void Out_Byte(BYTE b)
{
    if (s_oErr) return;
    s_oCrc = s_crcTab[(s_oCrc ^ b) & 0xFF] ^ (s_oCrc >> 8);
    s_oCount++;
    s_outBuf[s_outBufPos++] = b;
    if (s_outBufPos == OUT_BUF_SIZE) Out_Flush();
}

static DWORD Out_Crc()
{
    return s_oCrc ^ 0xFFFFFFFFUL;
}

// LZ77 window helpers
static void LZ_OutByte(BYTE b)
{
    s_lzWin[s_lzPos] = b;
    s_lzPos = (s_lzPos + 1) & LZ_WIN_MASK;
    Out_Byte(b);
}

static void LZ_OutRef(int offset, int length)
{
    int k;
    for (k = 0; k < length; ++k)
    {
        BYTE b = s_lzWin[offset];
        offset = (offset + 1) & LZ_WIN_MASK;
        LZ_OutByte(b);
    }
}

// LZSS window helpers
static void LS_OutByte(BYTE b)
{
    s_lsWin[s_lsPos] = b;
    s_lsPos = (s_lsPos + 1) & LS_WIN_MASK;
    Out_Byte(b);
}

static void LS_OutRef(int offset, int length)
{
    int k;
    for (k = 0; k < length; ++k)
    {
        BYTE b = s_lsWin[offset];
        offset = (offset + 1) & LS_WIN_MASK;
        LS_OutByte(b);
    }
}

// ============================================================================
// Compressed input buffer -- for reading block data from archive
// ============================================================================

#define COMP_BUF_SIZE  65536
static BYTE s_compBuf[COMP_BUF_SIZE];

// Block decode output buffer -- used by DecompressBlock and per-block x86 unfilter
static BYTE s_blockBuf[XBA_BLOCK_SIZE];

// ============================================================================
// Huffman intermediate buffer and decode table
// ============================================================================

static BYTE s_lzMidBuf[COMP_BUF_SIZE + COMP_BUF_SIZE / 4];

typedef struct { BYTE sym; BYTE len; } HuffEntry;
static HuffEntry s_huffTab[HUFF_TABLE_SIZE];
static int       s_huffPairLen[HUFF_SYMS];
static int       s_huffPairSym[HUFF_SYMS];
static BYTE      s_huffCodeLens[HUFF_SYMS];

// ============================================================================
// I/O helpers
// ============================================================================

static int ReadExact(HANDLE hf, void* buf, DWORD n)
{
    DWORD nr = 0;
    return ReadFile(hf, buf, n, &nr, NULL) && nr == n;
}

static int RD32(HANDLE hf, DWORD* out)
{
    return ReadExact(hf, out, 4);
}

static int RD16(HANDLE hf, WORD* out)
{
    return ReadExact(hf, out, 2);
}

// ============================================================================
// String / path helpers
// ============================================================================

static int XS_Len(const char* s)
{
    int n = 0; while (s[n]) ++n; return n;
}

static void XS_Copy(char* dst, int dstLen, const char* src)
{
    int i = 0;
    while (src[i] && i < dstLen - 1) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static void XS_Append(char* dst, int dstLen, const char* src)
{
    int i = XS_Len(dst);
    while (*src && i < dstLen - 1) dst[i++] = *src++;
    dst[i] = '\0';
}

static int BuildOutPath(char* buf, int blen,
    const char* dest, const char* rel, int relLen)
{
    int dl = XS_Len(dest);
    int i;
    if (dl + relLen + 1 >= blen) return 0;
    for (i = 0; i < dl; ++i)     buf[i] = dest[i];
    for (i = 0; i < relLen; ++i) buf[dl + i] = rel[i];
    buf[dl + relLen] = '\0';
    for (i = 0; buf[i]; ++i)
        if (buf[i] == '/') buf[i] = '\\';
    return 1;
}

static int EnsureDir(const char* path)
{
    char  buf[256];
    int   len = XS_Len(path);
    int   i;
    DWORD err;
    if (len >= 255) return 0;
    for (i = 0; i <= len; ++i) buf[i] = path[i];
    for (i = 1; i <= len; ++i)
    {
        if (buf[i] == '\\' || buf[i] == '\0')
        {
            char sv = buf[i];
            buf[i] = '\0';
            if (!CreateDirectoryA(buf, NULL))
            {
                err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS) return 0;
            }
            buf[i] = sv;
        }
    }
    return 1;
}

// ============================================================================
// LZ77 decompressor -- streams through Out_Byte/LZ_OutByte
// ============================================================================

static XbaResult LZ77_Decompress(const BYTE* src, DWORD srcLen, DWORD dstLen)
{
    DWORD spos = 0;
    DWORD ocount = 0;
    int   bit;
    int   tok;
    int   offset;
    int   length;
    BYTE  ctrl;

    while (ocount < dstLen && spos < srcLen)
    {
        ctrl = src[spos++];
        for (bit = 0; bit < 8; ++bit)
        {
            if (ocount >= dstLen || spos >= srcLen) break;
            if (ctrl & (1 << bit))
            {
                if (spos + 1 >= srcLen) break;
                tok = (int)src[spos] | ((int)src[spos + 1] << 8);
                spos += 2;
                offset = tok & 0x3FFF;
                length = ((tok >> 14) & 0xF) + LZ_MIN_MATCH;
                if (ocount + (DWORD)length > dstLen)
                    length = (int)(dstLen - ocount);
                LZ_OutRef(offset, length);
                ocount += (DWORD)length;
            }
            else
            {
                LZ_OutByte(src[spos++]);
                ocount++;
            }
            if (s_oErr) return XBA_ERR_WRITE;
        }
    }
    return (ocount == dstLen) ? XBA_OK : XBA_ERR_READ;
}

// ============================================================================
// LZSS decompressor -- streams through Out_Byte/LS_OutByte
// ============================================================================

static XbaResult LZSS_Decompress(const BYTE* src, DWORD srcLen, DWORD dstLen)
{
    DWORD spos = 0;
    DWORD ocount = 0;
    int   bit;
    int   tok;
    int   offset;
    int   length;
    BYTE  ctrl;

    while (ocount < dstLen && spos < srcLen)
    {
        ctrl = src[spos++];
        for (bit = 0; bit < 8; ++bit)
        {
            if (ocount >= dstLen || spos >= srcLen) break;
            if (ctrl & (1 << bit))
            {
                if (spos + 2 >= srcLen) break;
                tok = (int)src[spos]
                    | ((int)src[spos + 1] << 8)
                    | ((int)src[spos + 2] << 16);
                spos += 3;
                offset = tok & 0x7FFF;
                length = ((tok >> 15) & 0x3F) + LS_MIN_MATCH;
                if (ocount + (DWORD)length > dstLen)
                    length = (int)(dstLen - ocount);
                LS_OutRef(offset, length);
                ocount += (DWORD)length;
            }
            else
            {
                LS_OutByte(src[spos++]);
                ocount++;
            }
            if (s_oErr) return XBA_ERR_WRITE;
        }
    }
    return (ocount == dstLen) ? XBA_OK : XBA_ERR_READ;
}

// ============================================================================
// RLE decompressor -- streams through Out_Byte
// ============================================================================

static XbaResult RLE_Decompress(const BYTE* src, DWORD srcLen, DWORD dstLen)
{
    DWORD spos = 0;
    DWORD ocount = 0;
    int   count;
    int   k;
    BYTE  ctrl;
    BYTE  val;

    while (ocount < dstLen && spos < srcLen)
    {
        ctrl = src[spos++];
        count = (int)(ctrl & 0x7F) + 1;
        if (ctrl & 0x80)
        {
            for (k = 0; k < count && ocount < dstLen && spos < srcLen; ++k)
            {
                Out_Byte(src[spos++]);
                ocount++;
                if (s_oErr) return XBA_ERR_WRITE;
            }
        }
        else
        {
            if (spos >= srcLen) break;
            val = src[spos++];
            for (k = 0; k < count && ocount < dstLen; ++k)
            {
                Out_Byte(val);
                ocount++;
                if (s_oErr) return XBA_ERR_WRITE;
            }
        }
    }
    return (ocount == dstLen) ? XBA_OK : XBA_ERR_READ;
}

// ============================================================================
// Huffman decoder -- decodes to s_lzMidBuf, then calls LZ decompressor
// ============================================================================

static int Huff_BuildTable(const BYTE* codeLens)
{
    int pairCount = 0;
    int i, j, step, fill;
    int code, prevLen;
    int tmpLen, tmpSym;

    ZeroMemory(s_huffTab, sizeof(s_huffTab));

    for (i = 0; i < HUFF_SYMS; ++i)
    {
        if (codeLens[i] > 0 && codeLens[i] <= HUFF_MAX_BITS)
        {
            s_huffPairLen[pairCount] = (int)codeLens[i];
            s_huffPairSym[pairCount] = i;
            ++pairCount;
        }
    }

    if (pairCount == 0) return 1;

    for (i = 1; i < pairCount; ++i)
    {
        tmpLen = s_huffPairLen[i];
        tmpSym = s_huffPairSym[i];
        j = i - 1;
        while (j >= 0 && (s_huffPairLen[j] > tmpLen ||
            (s_huffPairLen[j] == tmpLen && s_huffPairSym[j] > tmpSym)))
        {
            s_huffPairLen[j + 1] = s_huffPairLen[j];
            s_huffPairSym[j + 1] = s_huffPairSym[j];
            --j;
        }
        s_huffPairLen[j + 1] = tmpLen;
        s_huffPairSym[j + 1] = tmpSym;
    }

    code = 0;
    prevLen = 0;
    for (i = 0; i < pairCount; ++i)
    {
        int len = s_huffPairLen[i];
        int sym = s_huffPairSym[i];
        if (prevLen == 0)
            code = 0;
        else
            code = (code + 1) << (len - prevLen);
        step = 1 << (HUFF_MAX_BITS - len);
        fill = code << (HUFF_MAX_BITS - len);
        while (fill < HUFF_TABLE_SIZE)
        {
            s_huffTab[fill].sym = (BYTE)sym;
            s_huffTab[fill].len = (BYTE)len;
            fill += step;
        }
        prevLen = len;
    }
    return 1;
}

static XbaResult Huff_Decompress(
    const BYTE* src, DWORD srcLen,
    DWORD dstLen,
    BYTE lzFlag)
{
    DWORD lzSize;
    DWORD opos;
    DWORD spos;
    DWORD bitBuf;
    int   bitAvail;
    int   idx;
    int   i;
    int   clen;

    if (srcLen <= (DWORD)(4 + HUFF_SYMS))
    {
        SetDetail("Huff block too short");
        return XBA_ERR_READ;
    }

    lzSize = (DWORD)src[0] | ((DWORD)src[1] << 8)
        | ((DWORD)src[2] << 16) | ((DWORD)src[3] << 24);

    if (lzSize == 0 || lzSize > (DWORD)sizeof(s_lzMidBuf))
    {
        SetDetailN("Huff lzSize bad=", (int)lzSize);
        return XBA_ERR_READ;
    }

    for (i = 0; i < HUFF_SYMS; ++i)
        s_huffCodeLens[i] = src[4 + i];

    if (!Huff_BuildTable(s_huffCodeLens))
    {
        SetDetail("Huff build failed");
        return XBA_ERR_READ;
    }

    spos = (DWORD)(4 + HUFF_SYMS);
    bitBuf = 0;
    bitAvail = 0;
    opos = 0;

    while (opos < lzSize)
    {
        while (bitAvail < HUFF_MAX_BITS && spos < srcLen)
        {
            bitBuf |= ((DWORD)src[spos++]) << bitAvail;
            bitAvail += 8;
        }
        idx = (int)(bitBuf & (DWORD)(HUFF_TABLE_SIZE - 1));
        clen = (int)s_huffTab[idx].len;
        if (clen == 0)
        {
            SetDetailN("Huff invalid code opos=", (int)opos);
            return XBA_ERR_READ;
        }
        s_lzMidBuf[opos++] = s_huffTab[idx].sym;
        bitBuf >>= clen;
        bitAvail -= clen;
    }

    if (lzFlag == V2_BLK_LZ77)
        return LZ77_Decompress(s_lzMidBuf, lzSize, dstLen);
    else
        return LZSS_Decompress(s_lzMidBuf, lzSize, dstLen);
}

// ============================================================================
// V2 block dispatcher
// ============================================================================

// DecompressBlockToBuf -- decompress to a RAM buffer (used for x86-filtered blocks
// so we can unfilter in-place before streaming through Out_Byte).
// Mirrors the old buffer-based decompressor exactly.
static XbaResult DecompressBlockToBuf(
    BYTE blockFlag,
    const BYTE* src, DWORD srcLen,
    BYTE* dst, DWORD dstLen)
{
    DWORD spos, opos, wpos, j;
    int   bit, tok, offset, length, k;
    BYTE  ctrl, b;
    DWORD lzSize, spos2, bitBuf, opos2;
    int   bitAvail, idx, clen, i;

    switch (blockFlag)
    {
    case V2_BLK_STORED:
        if (srcLen != dstLen) { SetDetail("Stored size mismatch"); return XBA_ERR_READ; }
        CopyMemory(dst, src, dstLen);
        return XBA_OK;

    case V2_BLK_LZ77:
        spos = 0; opos = 0; wpos = 0;
        ZeroMemory(s_lzWin, sizeof(s_lzWin));
        while (opos < dstLen && spos < srcLen)
        {
            ctrl = src[spos++];
            for (bit = 0; bit < 8; ++bit)
            {
                if (opos >= dstLen || spos >= srcLen) break;
                if (ctrl & (1 << bit))
                {
                    if (spos + 1 >= srcLen) break;
                    tok = (int)src[spos] | ((int)src[spos + 1] << 8); spos += 2;
                    offset = tok & 0x3FFF;
                    length = ((tok >> 14) & 0xF) + LZ_MIN_MATCH;
                    for (k = 0; k < length && opos < dstLen; ++k)
                    {
                        b = s_lzWin[offset]; offset = (offset + 1) & LZ_WIN_MASK;
                        s_lzWin[wpos] = b; wpos = (wpos + 1) & LZ_WIN_MASK;
                        dst[opos++] = b;
                    }
                }
                else
                {
                    b = src[spos++];
                    s_lzWin[wpos] = b; wpos = (wpos + 1) & LZ_WIN_MASK;
                    dst[opos++] = b;
                }
            }
        }
        return (opos == dstLen) ? XBA_OK : XBA_ERR_READ;

    case V2_BLK_LZSS:
        spos = 0; opos = 0; wpos = 0;
        ZeroMemory(s_lsWin, sizeof(s_lsWin));
        while (opos < dstLen && spos < srcLen)
        {
            ctrl = src[spos++];
            for (bit = 0; bit < 8; ++bit)
            {
                if (opos >= dstLen || spos >= srcLen) break;
                if (ctrl & (1 << bit))
                {
                    if (spos + 2 >= srcLen) break;
                    tok = (int)src[spos] | ((int)src[spos + 1] << 8) | ((int)src[spos + 2] << 16); spos += 3;
                    offset = tok & 0x7FFF;
                    length = ((tok >> 15) & 0x3F) + LS_MIN_MATCH;
                    for (k = 0; k < length && opos < dstLen; ++k)
                    {
                        b = s_lsWin[offset]; offset = (offset + 1) & LS_WIN_MASK;
                        s_lsWin[wpos] = b; wpos = (wpos + 1) & LS_WIN_MASK;
                        dst[opos++] = b;
                    }
                }
                else
                {
                    b = src[spos++];
                    s_lsWin[wpos] = b; wpos = (wpos + 1) & LS_WIN_MASK;
                    dst[opos++] = b;
                }
            }
        }
        return (opos == dstLen) ? XBA_OK : XBA_ERR_READ;

    case V2_BLK_RLE:
        spos = 0; opos = 0;
        while (opos < dstLen && spos < srcLen)
        {
            ctrl = src[spos++];
            k = (int)(ctrl & 0x7F) + 1;
            if (ctrl & 0x80)
            {
                for (j = 0; (int)j < k && opos < dstLen && spos < srcLen; ++j)
                    dst[opos++] = src[spos++];
            }
            else
            {
                if (spos >= srcLen) break;
                b = src[spos++];
                for (j = 0; (int)j < k && opos < dstLen; ++j)
                    dst[opos++] = b;
            }
        }
        return (opos == dstLen) ? XBA_OK : XBA_ERR_READ;

    case V2_BLK_LZ77_HUFF:
    case V2_BLK_LZSS_HUFF:
        if (srcLen <= (DWORD)(4 + HUFF_SYMS)) { SetDetail("Huff block too short"); return XBA_ERR_READ; }
        lzSize = (DWORD)src[0] | ((DWORD)src[1] << 8) | ((DWORD)src[2] << 16) | ((DWORD)src[3] << 24);
        if (lzSize == 0 || lzSize > (DWORD)sizeof(s_lzMidBuf)) { SetDetailN("Huff lzSize bad=", (int)lzSize); return XBA_ERR_READ; }
        for (i = 0; i < HUFF_SYMS; ++i) s_huffCodeLens[i] = src[4 + i];
        if (!Huff_BuildTable(s_huffCodeLens)) { SetDetail("Huff build failed"); return XBA_ERR_READ; }
        spos2 = (DWORD)(4 + HUFF_SYMS); bitBuf = 0; bitAvail = 0; opos2 = 0;
        while (opos2 < lzSize)
        {
            while (bitAvail < HUFF_MAX_BITS && spos2 < srcLen) { bitBuf |= ((DWORD)src[spos2++]) << bitAvail; bitAvail += 8; }
            idx = (int)(bitBuf & (DWORD)(HUFF_TABLE_SIZE - 1));
            clen = (int)s_huffTab[idx].len;
            if (clen == 0) { SetDetailN("Huff invalid code=", (int)opos2); return XBA_ERR_READ; }
            s_lzMidBuf[opos2++] = s_huffTab[idx].sym;
            bitBuf >>= clen; bitAvail -= clen;
        }
        // Now LZ decompress s_lzMidBuf -> dst
        if (blockFlag == V2_BLK_LZ77_HUFF)
        {
            spos = 0; opos = 0; wpos = 0;
            ZeroMemory(s_lzWin, sizeof(s_lzWin));
            while (opos < dstLen && spos < lzSize)
            {
                ctrl = s_lzMidBuf[spos++];
                for (bit = 0; bit < 8; ++bit)
                {
                    if (opos >= dstLen || spos >= lzSize) break;
                    if (ctrl & (1 << bit))
                    {
                        if (spos + 1 >= lzSize) break;
                        tok = (int)s_lzMidBuf[spos] | ((int)s_lzMidBuf[spos + 1] << 8); spos += 2;
                        offset = tok & 0x3FFF; length = ((tok >> 14) & 0xF) + LZ_MIN_MATCH;
                        for (k = 0; k < length && opos < dstLen; ++k)
                        {
                            b = s_lzWin[offset]; offset = (offset + 1) & LZ_WIN_MASK;
                            s_lzWin[wpos] = b; wpos = (wpos + 1) & LZ_WIN_MASK; dst[opos++] = b;
                        }
                    }
                    else { b = s_lzMidBuf[spos++]; s_lzWin[wpos] = b; wpos = (wpos + 1) & LZ_WIN_MASK; dst[opos++] = b; }
                }
            }
        }
        else
        {
            spos = 0; opos = 0; wpos = 0;
            ZeroMemory(s_lsWin, sizeof(s_lsWin));
            while (opos < dstLen && spos < lzSize)
            {
                ctrl = s_lzMidBuf[spos++];
                for (bit = 0; bit < 8; ++bit)
                {
                    if (opos >= dstLen || spos >= lzSize) break;
                    if (ctrl & (1 << bit))
                    {
                        if (spos + 2 >= lzSize) break;
                        tok = (int)s_lzMidBuf[spos] | ((int)s_lzMidBuf[spos + 1] << 8) | ((int)s_lzMidBuf[spos + 2] << 16); spos += 3;
                        offset = tok & 0x7FFF; length = ((tok >> 15) & 0x3F) + LS_MIN_MATCH;
                        for (k = 0; k < length && opos < dstLen; ++k)
                        {
                            b = s_lsWin[offset]; offset = (offset + 1) & LS_WIN_MASK;
                            s_lsWin[wpos] = b; wpos = (wpos + 1) & LS_WIN_MASK; dst[opos++] = b;
                        }
                    }
                    else { b = s_lzMidBuf[spos++]; s_lsWin[wpos] = b; wpos = (wpos + 1) & LS_WIN_MASK; dst[opos++] = b; }
                }
            }
        }
        return (opos == dstLen) ? XBA_OK : XBA_ERR_READ;

    default:
        SetDetailN("Unknown block flag=", (int)blockFlag);
        return XBA_ERR_READ;
    }
    (void)j; (void)spos2; (void)opos2; (void)bitBuf; (void)bitAvail; (void)idx; (void)clen; (void)i; (void)lzSize;
}

static XbaResult DecompressBlock(
    BYTE blockFlag,
    const BYTE* src, DWORD srcLen,
    DWORD dstLen)
{
    DWORD i;
    switch (blockFlag)
    {
    case V2_BLK_STORED:
        if (srcLen != dstLen) { SetDetail("Stored size mismatch"); return XBA_ERR_READ; }
        for (i = 0; i < dstLen; ++i)
        {
            Out_Byte(src[i]);
            if (s_oErr) return XBA_ERR_WRITE;
        }
        return XBA_OK;
    case V2_BLK_LZ77:
        return LZ77_Decompress(src, srcLen, dstLen);
    case V2_BLK_LZSS:
        return LZSS_Decompress(src, srcLen, dstLen);
    case V2_BLK_RLE:
        return RLE_Decompress(src, srcLen, dstLen);
    case V2_BLK_LZ77_HUFF:
        return Huff_Decompress(src, srcLen, dstLen, V2_BLK_LZ77);
    case V2_BLK_LZSS_HUFF:
        return Huff_Decompress(src, srcLen, dstLen, V2_BLK_LZSS);
    default:
        SetDetailN("Unknown block flag=", (int)blockFlag);
        return XBA_ERR_READ;
    }
}

// ============================================================================
// x86 unfilter -- in-memory on a small static buffer, one byte at a time
// For v1: called on s_v1Buf after decompression
// For v2: NOT USED -- x86 filter requires whole-file context which we no
//         longer have since we stream directly. V2 x86 files are stored
//         pre-filtered by the packer; we skip the unfilter pass.
//         NOTE: update packer to not apply x86 filter for v2 if targeting Xbox.
// ============================================================================

static void X86_Unfilter(BYTE* data, DWORD len)
{
    DWORD i;
    DWORD abs_val;
    DWORD rel;
    if (len < 5) return;
    for (i = 0; i + 4 < len; ++i)
    {
        if (data[i] != 0xE8 && data[i] != 0xE9) continue;
        abs_val = (DWORD)data[i + 1] | ((DWORD)data[i + 2] << 8)
            | ((DWORD)data[i + 3] << 16) | ((DWORD)data[i + 4] << 24);
        rel = abs_val - (i + 5);
        data[i + 1] = (BYTE)(rel);
        data[i + 2] = (BYTE)(rel >> 8);
        data[i + 3] = (BYTE)(rel >> 16);
        data[i + 4] = (BYTE)(rel >> 24);
        i += 4;
    }
}

// v1 files are small (< XBA_BLOCK_SIZE) -- decompress to this buffer
static BYTE s_v1Buf[XBA_BLOCK_SIZE];

// ============================================================================
// Pre-scan helpers
// ============================================================================

static int CountFiles_V1(HANDLE hf, DWORD entryCount)
{
    int   total = 0;
    DWORD i;
    BYTE  sf, spl;
    DWORD scomp;
    DWORD dummy;
    for (i = 0; i < entryCount; ++i)
    {
        if (!ReadExact(hf, &sf, 1)) break;
        if (!ReadExact(hf, &spl, 1)) break;
        SetFilePointer(hf, spl, NULL, FILE_CURRENT);
        if (sf != V1_FLAG_DIR)
        {
            if (!RD32(hf, &dummy)) break;
            if (!RD32(hf, &scomp)) break;
            if (!RD32(hf, &dummy)) break;
            SetFilePointer(hf, (LONG)scomp, NULL, FILE_CURRENT);
            total++;
        }
    }
    return total;
}

static int CountFiles_V2(HANDLE hf, DWORD entryCount)
{
    int   total = 0;
    DWORD i, bi;
    BYTE  sf, spl;
    WORD  blockCount;
    DWORD dummy, scomp;
    BYTE  bflag;
    for (i = 0; i < entryCount; ++i)
    {
        if (!ReadExact(hf, &sf, 1)) break;
        if (!ReadExact(hf, &spl, 1)) break;
        SetFilePointer(hf, spl, NULL, FILE_CURRENT);
        if (sf != V2_FLAG_DIR)
        {
            if (!RD32(hf, &dummy))      break;
            if (!RD32(hf, &dummy))      break;
            if (!RD16(hf, &blockCount)) break;
            for (bi = 0; bi < (DWORD)blockCount; ++bi)
            {
                if (!ReadExact(hf, &bflag, 1)) goto done_v2;
                if (!RD32(hf, &scomp))         goto done_v2;
                SetFilePointer(hf, (LONG)scomp, NULL, FILE_CURRENT);
            }
            total++;
        }
    }
done_v2:
    return total;
}

// ============================================================================
// Extract_V1
// ============================================================================

static XbaResult Extract_V1(
    HANDLE hf, DWORD entryCount,
    const char* destFolder,
    XbaProgressFn progressFn,
    int* filesDone, int filesTotal, DWORD* bytesDone)
{
    DWORD     ei;
    BYTE      flags;
    BYTE      pathLen;
    char      relPath[256];
    DWORD     uncompSize;
    DWORD     compSize;
    DWORD     storedCrc;
    DWORD     actualCrc;
    char      outPath[256];
    char      dirBuf[256];
    int       di;
    int       lastSep;
    XbaResult dr;
    HANDLE    hOut;
    DWORD     nw;
    DWORD     nr;

    for (ei = 0; ei < entryCount; ++ei)
    {
        if (!ReadExact(hf, &flags, 1)) { SetDetail("V1 read flags");   return XBA_ERR_READ; }
        if (!ReadExact(hf, &pathLen, 1)) { SetDetail("V1 read pathLen"); return XBA_ERR_READ; }
        if (!ReadExact(hf, relPath, (DWORD)pathLen))
        {
            SetDetail("V1 read path"); return XBA_ERR_READ;
        }
        relPath[pathLen] = '\0';

        if (flags == V1_FLAG_DIR)
        {
            if (!BuildOutPath(outPath, sizeof(outPath), destFolder, relPath, pathLen))
            {
                SetDetailN("V1 dir overflow ei=", (int)ei); return XBA_ERR_CREATE;
            }
            if (!EnsureDir(outPath))
            {
                SetDetail("V1 EnsureDir failed"); return XBA_ERR_CREATE;
            }
        }
        else
        {
            if (!RD32(hf, &uncompSize)) { SetDetail("V1 read uncompSize"); return XBA_ERR_READ; }
            if (!RD32(hf, &compSize)) { SetDetail("V1 read compSize");   return XBA_ERR_READ; }
            if (!RD32(hf, &storedCrc)) { SetDetail("V1 read crc");        return XBA_ERR_READ; }

            if (!BuildOutPath(outPath, sizeof(outPath), destFolder, relPath, pathLen))
            {
                SetDetailN("V1 path overflow ei=", (int)ei); return XBA_ERR_CREATE;
            }

            XS_Copy(dirBuf, sizeof(dirBuf), outPath);
            lastSep = -1;
            for (di = 0; dirBuf[di]; ++di)
                if (dirBuf[di] == '\\') lastSep = di;
            if (lastSep >= 0) { dirBuf[lastSep + 1] = '\0'; if (!EnsureDir(dirBuf)) { SetDetail("V1 EnsureDir file failed"); return XBA_ERR_CREATE; } }

            hOut = CreateFileA(outPath, GENERIC_WRITE, 0,
                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hOut == INVALID_HANDLE_VALUE)
            {
                SetDetail("V1 create output"); return XBA_ERR_CREATE;
            }

            if (compSize == uncompSize)
            {
                // Stored -- stream in chunks, supports any file size
                DWORD rem = compSize;
                DWORD crc = 0xFFFFFFFFUL;
                while (rem > 0)
                {
                    DWORD chunk = rem < OUT_BUF_SIZE ? rem : OUT_BUF_SIZE;
                    DWORD nr2 = 0, nw2 = 0, ci;
                    if (!ReadFile(hf, s_outBuf, chunk, &nr2, NULL) || nr2 != chunk)
                    {
                        CloseHandle(hOut); DeleteFileA(outPath); SetDetailN("V1 store read ei=", (int)ei); return XBA_ERR_READ;
                    }
                    for (ci = 0; ci < nr2; ++ci)
                        crc = s_crcTab[(crc ^ s_outBuf[ci]) & 0xFF] ^ (crc >> 8);
                    if (!WriteFile(hOut, s_outBuf, nr2, &nw2, NULL) || nw2 != nr2)
                    {
                        CloseHandle(hOut); DeleteFileA(outPath); SetDetailN("V1 store write ei=", (int)ei); return XBA_ERR_WRITE;
                    }
                    rem -= nr2;
                }
                actualCrc = crc ^ 0xFFFFFFFFUL;
                if (actualCrc != storedCrc)
                {
                    CloseHandle(hOut); DeleteFileA(outPath); SetDetailN("V1 CRC fail ei=", (int)ei); return XBA_ERR_CRC;
                }
                FlushFileBuffers(hOut);
                CloseHandle(hOut);
            }
            else
            {
                // Compressed -- LZ window bounded, must fit in static buffers
                if (compSize > XBA_BLOCK_SIZE || uncompSize > XBA_BLOCK_SIZE)
                {
                    CloseHandle(hOut); DeleteFileA(outPath); SetDetailN("V1 comp too large ei=", (int)ei); return XBA_ERR_MEM;
                }

                if (!ReadExact(hf, s_compBuf, compSize))
                {
                    CloseHandle(hOut); DeleteFileA(outPath); SetDetailN("V1 read data ei=", (int)ei); return XBA_ERR_READ;
                }

                if (flags == V1_FLAG_LZSS || flags == V1_FLAG_X86_LZSS)
                {
                    DWORD spos = 0, opos = 0, wpos = 0;
                    int   bit, tok, offset, length, k;
                    BYTE  ctrl, b;
                    ZeroMemory(s_lsWin, sizeof(s_lsWin));
                    while (opos < uncompSize && spos < compSize)
                    {
                        ctrl = s_compBuf[spos++];
                        for (bit = 0; bit < 8; ++bit)
                        {
                            if (opos >= uncompSize || spos >= compSize) break;
                            if (ctrl & (1 << bit))
                            {
                                if (spos + 2 >= compSize) break;
                                tok = (int)s_compBuf[spos] | ((int)s_compBuf[spos + 1] << 8) | ((int)s_compBuf[spos + 2] << 16);
                                spos += 3;
                                offset = tok & 0x7FFF;
                                length = ((tok >> 15) & 0x3F) + LS_MIN_MATCH;
                                for (k = 0; k < length && opos < uncompSize; ++k)
                                {
                                    b = s_lsWin[offset]; offset = (offset + 1) & LS_WIN_MASK;
                                    s_lsWin[wpos] = b; wpos = (wpos + 1) & LS_WIN_MASK;
                                    s_v1Buf[opos++] = b;
                                }
                            }
                            else
                            {
                                b = s_compBuf[spos++];
                                s_lsWin[wpos] = b; wpos = (wpos + 1) & LS_WIN_MASK;
                                s_v1Buf[opos++] = b;
                            }
                        }
                    }
                    if (opos != uncompSize) { CloseHandle(hOut); DeleteFileA(outPath); SetDetailN("V1 LZSS fail ei=", (int)ei); return XBA_ERR_READ; }
                }
                else
                {
                    DWORD spos = 0, opos = 0, wpos = 0;
                    int   bit, tok, offset, length, k;
                    BYTE  ctrl, b;
                    ZeroMemory(s_lzWin, sizeof(s_lzWin));
                    while (opos < uncompSize && spos < compSize)
                    {
                        ctrl = s_compBuf[spos++];
                        for (bit = 0; bit < 8; ++bit)
                        {
                            if (opos >= uncompSize || spos >= compSize) break;
                            if (ctrl & (1 << bit))
                            {
                                if (spos + 1 >= compSize) break;
                                tok = (int)s_compBuf[spos] | ((int)s_compBuf[spos + 1] << 8);
                                spos += 2;
                                offset = tok & 0x3FFF;
                                length = ((tok >> 14) & 0xF) + LZ_MIN_MATCH;
                                for (k = 0; k < length && opos < uncompSize; ++k)
                                {
                                    b = s_lzWin[offset]; offset = (offset + 1) & LZ_WIN_MASK;
                                    s_lzWin[wpos] = b; wpos = (wpos + 1) & LZ_WIN_MASK;
                                    s_v1Buf[opos++] = b;
                                }
                            }
                            else
                            {
                                b = s_compBuf[spos++];
                                s_lzWin[wpos] = b; wpos = (wpos + 1) & LZ_WIN_MASK;
                                s_v1Buf[opos++] = b;
                            }
                        }
                    }
                    if (opos != uncompSize) { CloseHandle(hOut); DeleteFileA(outPath); SetDetailN("V1 LZ77 fail ei=", (int)ei); return XBA_ERR_READ; }
                }

                if (flags == V1_FLAG_X86_LZ77 || flags == V1_FLAG_X86_LZSS)
                    X86_Unfilter(s_v1Buf, uncompSize);

                {
                    DWORD crc = 0xFFFFFFFFUL, ci;
                    for (ci = 0; ci < uncompSize; ++ci)
                        crc = s_crcTab[(crc ^ s_v1Buf[ci]) & 0xFF] ^ (crc >> 8);
                    actualCrc = crc ^ 0xFFFFFFFFUL;
                }
                if (actualCrc != storedCrc)
                {
                    CloseHandle(hOut); DeleteFileA(outPath); SetDetailN("V1 CRC fail ei=", (int)ei); return XBA_ERR_CRC;
                }

                nw = 0;
                if (!WriteFile(hOut, s_v1Buf, uncompSize, &nw, NULL) || nw != uncompSize)
                {
                    CloseHandle(hOut); DeleteFileA(outPath); SetDetailN("V1 write fail ei=", (int)ei); return XBA_ERR_WRITE;
                }
                FlushFileBuffers(hOut);
                CloseHandle(hOut);
            }

            ++(*filesDone);
            *bytesDone += uncompSize;
            if (progressFn)
                progressFn(*filesDone, filesTotal, *bytesDone, 0);
        }
    }
    (void)dr; (void)nr;
    return XBA_OK;
}

// ============================================================================
// Extract_V2
// ============================================================================

static XbaResult Extract_V2(
    HANDLE hf, DWORD entryCount,
    const char* destFolder,
    XbaProgressFn progressFn,
    int* filesDone, int filesTotal, DWORD* bytesDone)
{
    DWORD     ei, bi;
    BYTE      fileFlag;
    BYTE      pathLen;
    char      relPath[256];
    DWORD     uncompSize;
    DWORD     storedCrc;
    DWORD     actualCrc;
    WORD      blockCount;
    BYTE      blockFlag;
    DWORD     compSize;
    DWORD     blockOutSize;
    DWORD     baseBytes;
    DWORD     x86GlobalI;
    char      outPath[256];
    char      dirBuf[256];
    int       di;
    int       lastSep;
    XbaResult dr;
    HANDLE    hOut;

    for (ei = 0; ei < entryCount; ++ei)
    {
        if (!ReadExact(hf, &fileFlag, 1)) { SetDetail("V2 read fileFlag");  return XBA_ERR_READ; }
        if (!ReadExact(hf, &pathLen, 1)) { SetDetail("V2 read pathLen");   return XBA_ERR_READ; }
        if (!ReadExact(hf, relPath, (DWORD)pathLen))
        {
            SetDetail("V2 read path"); return XBA_ERR_READ;
        }
        relPath[pathLen] = '\0';

        if (fileFlag == V2_FLAG_DIR)
        {
            if (!BuildOutPath(outPath, sizeof(outPath), destFolder, relPath, pathLen))
            {
                SetDetailN("V2 dir overflow ei=", (int)ei); return XBA_ERR_CREATE;
            }
            if (!EnsureDir(outPath))
            {
                SetDetail("V2 EnsureDir failed"); return XBA_ERR_CREATE;
            }
        }
        else
        {
            if (fileFlag != V2_FLAG_FILE && fileFlag != V2_FLAG_X86)
            {
                SetDetailN("V2 bad fileFlag ei=", (int)ei); return XBA_ERR_READ;
            }

            if (!RD32(hf, &uncompSize)) { SetDetail("V2 read uncompSize"); return XBA_ERR_READ; }
            if (!RD32(hf, &storedCrc)) { SetDetail("V2 read crc");        return XBA_ERR_READ; }
            if (!RD16(hf, &blockCount)) { SetDetail("V2 read blockCount"); return XBA_ERR_READ; }

            if (blockCount == 0)
            {
                SetDetailN("V2 zero blockCount ei=", (int)ei); return XBA_ERR_READ;
            }

            if (!BuildOutPath(outPath, sizeof(outPath), destFolder, relPath, pathLen))
            {
                SetDetailN("V2 path overflow ei=", (int)ei); return XBA_ERR_CREATE;
            }

            XS_Copy(dirBuf, sizeof(dirBuf), outPath);
            lastSep = -1;
            for (di = 0; dirBuf[di]; ++di)
                if (dirBuf[di] == '\\') lastSep = di;
            if (lastSep >= 0)
            {
                dirBuf[lastSep + 1] = '\0';
                if (!EnsureDir(dirBuf))
                {
                    SetDetail("V2 EnsureDir file failed"); return XBA_ERR_CREATE;
                }
            }

            hOut = CreateFileA(outPath, GENERIC_WRITE, 0,
                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hOut == INVALID_HANDLE_VALUE)
            {
                SetDetail("V2 create output"); return XBA_ERR_CREATE;
            }

            Out_Init(hOut);
            dr = XBA_OK;
            x86GlobalI = 0;

            for (bi = 0; bi < (DWORD)blockCount; ++bi)
            {
                DWORD j;
                DWORD abs_val;
                DWORD rel_val;

                if (!ReadExact(hf, &blockFlag, 1))
                {
                    SetDetail("V2 read blockFlag"); dr = XBA_ERR_READ; break;
                }

                if (!RD32(hf, &compSize))
                {
                    SetDetail("V2 read compSize"); dr = XBA_ERR_READ; break;
                }

                if (compSize > (DWORD)sizeof(s_compBuf))
                {
                    SetDetailN("V2 block too large bi=", (int)bi); dr = XBA_ERR_MEM; break;
                }

                if (bi < (DWORD)blockCount - 1)
                {
                    blockOutSize = XBA_BLOCK_SIZE;
                }
                else
                {
                    baseBytes = (DWORD)(blockCount - 1) * XBA_BLOCK_SIZE;
                    if (uncompSize <= baseBytes)
                    {
                        SetDetailN("V2 size underflow ei=", (int)ei); dr = XBA_ERR_READ; break;
                    }
                    blockOutSize = uncompSize - baseBytes;
                    if (blockOutSize > XBA_BLOCK_SIZE)
                    {
                        SetDetailN("V2 bad last block ei=", (int)ei); dr = XBA_ERR_READ; break;
                    }
                }

                if (!ReadExact(hf, s_compBuf, compSize))
                {
                    SetDetailN("V2 read block bi=", (int)bi); dr = XBA_ERR_READ; break;
                }

                if (fileFlag == V2_FLAG_X86)
                {
                    // x86 file: decompress to s_blockBuf, unfilter in-place,
                    // then stream through Out_Byte.
                    dr = DecompressBlockToBuf(blockFlag,
                        s_compBuf, compSize, s_blockBuf, blockOutSize);
                    if (dr != XBA_OK)
                    {
                        SetDetailN("V2 decomp fail bi=", (int)bi); break;
                    }

                    if (blockOutSize >= 5)
                    {
                        for (j = 0; j + 4 < blockOutSize; ++j)
                        {
                            if (s_blockBuf[j] != 0xE8 && s_blockBuf[j] != 0xE9) continue;
                            abs_val = (DWORD)s_blockBuf[j + 1]
                                | ((DWORD)s_blockBuf[j + 2] << 8)
                                | ((DWORD)s_blockBuf[j + 3] << 16)
                                | ((DWORD)s_blockBuf[j + 4] << 24);
                            rel_val = abs_val - (x86GlobalI + j + 5);
                            s_blockBuf[j + 1] = (BYTE)(rel_val);
                            s_blockBuf[j + 2] = (BYTE)(rel_val >> 8);
                            s_blockBuf[j + 3] = (BYTE)(rel_val >> 16);
                            s_blockBuf[j + 4] = (BYTE)(rel_val >> 24);
                            j += 4;
                        }
                    }
                    x86GlobalI += blockOutSize;

                    {
                        DWORD oi;
                        for (oi = 0; oi < blockOutSize; ++oi)
                        {
                            Out_Byte(s_blockBuf[oi]);
                            if (s_oErr) { dr = XBA_ERR_WRITE; break; }
                        }
                        if (dr != XBA_OK) break;
                    }
                }
                else
                {
                    // Non-x86: stream directly through Out_Byte (no buf needed)
                    dr = DecompressBlock(blockFlag, s_compBuf, compSize, blockOutSize);
                    if (dr != XBA_OK)
                    {
                        SetDetailN("V2 decomp fail bi=", (int)bi); break;
                    }
                    x86GlobalI += blockOutSize;
                }

                if (progressFn && (bi & 7) == 7)
                    progressFn(*filesDone, filesTotal,
                        *bytesDone + s_oCount, 0);
            }

            // Validate total bytes written
            if (dr == XBA_OK && s_oCount != uncompSize)
            {
                SetDetailN("V2 size mismatch ei=", (int)ei);
                dr = XBA_ERR_READ;
            }

            // CRC over output bytes (tracked by Out_Byte via s_oCrc)
            if (dr == XBA_OK)
            {
                actualCrc = Out_Crc();
                if (actualCrc != storedCrc)
                {
                    SetDetailN("V2 CRC fail ei=", (int)ei);
                    dr = XBA_ERR_CRC;
                }
            }

            Out_Flush();
            FlushFileBuffers(hOut);
            CloseHandle(hOut);

            if (dr != XBA_OK)
            {
                DeleteFileA(outPath); return dr;
            }

            ++(*filesDone);
            *bytesDone += uncompSize;
            if (progressFn)
                progressFn(*filesDone, filesTotal, *bytesDone, 0);
        }
    }
    return XBA_OK;
}

// ============================================================================
// Xba_Extract
// ============================================================================

XbaResult Xba_Extract(
    const char* xbaPath,
    const char* destFolder,
    XbaProgressFn progressFn,
    char* detailOut,
    int           detailLen)
{
    HANDLE    hf = INVALID_HANDLE_VALUE;
    BYTE      magic[4];
    DWORD     entryCount;
    int       filesDone = 0;
    int       filesTotal = 0;
    DWORD     bytesDone = 0;
    int       isV2 = 0;
    XbaResult ret = XBA_OK;

    s_detail[0] = '\0';
    CRC_Init();

    hf = CreateFileA(xbaPath, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        SetDetail("CreateFile failed"); ret = XBA_ERR_OPEN; goto done;
    }

    if (!ReadExact(hf, magic, 4))
    {
        SetDetail("Read magic failed"); ret = XBA_ERR_READ; goto done;
    }

    if (magic[0] == k_magic_v2[0] && magic[1] == k_magic_v2[1] &&
        magic[2] == k_magic_v2[2] && magic[3] == k_magic_v2[3])
        isV2 = 1;
    else if (magic[0] == k_magic_v1[0] && magic[1] == k_magic_v1[1] &&
        magic[2] == k_magic_v1[2] && magic[3] == k_magic_v1[3])
        isV2 = 0;
    else
    {
        SetDetail("Bad magic"); ret = XBA_ERR_MAGIC; goto done;
    }

    if (!RD32(hf, &entryCount))
    {
        SetDetail("Read entry_count failed"); ret = XBA_ERR_READ; goto done;
    }

    {
        DWORD savePosLow;
        LONG  savePosHigh = 0;
        savePosLow = SetFilePointer(hf, 0, &savePosHigh, FILE_CURRENT);
        filesTotal = isV2
            ? CountFiles_V2(hf, entryCount)
            : CountFiles_V1(hf, entryCount);
        SetFilePointer(hf, (LONG)savePosLow, &savePosHigh, FILE_BEGIN);
    }

    ret = isV2
        ? Extract_V2(hf, entryCount, destFolder, progressFn,
            &filesDone, filesTotal, &bytesDone)
        : Extract_V1(hf, entryCount, destFolder, progressFn,
            &filesDone, filesTotal, &bytesDone);

done:
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    if (detailOut) CopyDetail(detailOut, detailLen);
    return ret;
}

// ============================================================================
// Xba_ResultStr
// ============================================================================

const char* Xba_ResultStr(XbaResult r)
{
    switch (r)
    {
    case XBA_OK:         return "OK";
    case XBA_ERR_OPEN:   return "Cannot open archive";
    case XBA_ERR_MAGIC:  return "Not an XBA archive";
    case XBA_ERR_READ:   return "Read error";
    case XBA_ERR_CREATE: return "Cannot create file";
    case XBA_ERR_WRITE:  return "Write error";
    case XBA_ERR_CRC:    return "CRC mismatch";
    case XBA_ERR_MEM:    return "File too large for buffer";
    default:             return "Unknown error";
    }
}