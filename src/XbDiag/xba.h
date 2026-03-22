#pragma once
// xba.h
// XbDiag - XBA archive extractor.  Supports v1 (XBA\x01) and v2 (XBA\x02).
//
// ============================================================================
// XBA v1 format  (magic XBA\x01)
// ============================================================================
//
//   Header:    magic[4]  entry_count[4 LE]
//   Per entry: flags[1]  path_len[1]  path[N]
//              if file:  uncomp_size[4 LE]  comp_size[4 LE]  crc32[4 LE]
//                        data[comp_size]
//
//   file flags (v1):
//     0x01 = directory
//     0x00 = file, LZ77 or stored
//     0x02 = file, x86-filtered + LZ77 or stored
//     0x03 = file, LZSS or stored
//     0x04 = file, x86-filtered + LZSS or stored
//
//   comp_size == uncomp_size => stored.
//   CRC32 is of the original unfiltered data.
//
//   LZ77 token (2 bytes):
//     bits  0-13 : window offset (16 KB window)
//     bits 14-17 : length - 3   (lengths 3-18)
//
//   LZSS token (3 bytes):
//     bits  0-14 : window offset (32 KB window)
//     bits 15-20 : length - 2   (lengths 2-65)
//
// ============================================================================
// XBA v2 format  (magic XBA\x02)
// ============================================================================
//
//   Header:    magic[4]  entry_count[4 LE]
//   Per entry: file_flag[1]  path_len[1]  path[N]
//              if file:  uncomp_size[4 LE]  crc32[4 LE]
//                        block_count[2 LE]
//                        blocks[block_count]:
//                          block_flag[1]  comp_size[4 LE]  data[comp_size]
//
//   file_flag (v2):
//     0x01 = directory
//     0x00 = file, no x86 filter
//     0x02 = file, x86 filter applied to whole file before blocking
//
//   block_flag (v2):
//     0x00 = stored
//     0x01 = LZ77
//     0x02 = LZSS
//     0x03 = RLE
//     0x04 = LZ77 + Huffman
//     0x05 = LZSS + Huffman
//
//   Each block decompresses to XBA_BLOCK_SIZE bytes except the last which
//   decompresses to (uncomp_size - (block_count-1) * XBA_BLOCK_SIZE).
//
//   x86 unfilter is applied once to the full reconstructed file after all
//   blocks are decoded, when file_flag == 0x02.
//
//   CRC32 is of the final decoded + unfiltered data.
//
//   Huffman block layout (block_flag 0x04 / 0x05):
//     lz_size[4 LE]         size in bytes of the LZ-compressed intermediate
//     code_lengths[256]     one byte per symbol; 0 = symbol absent
//     bitstream[...]        remainder of comp_size bytes, packed LSB-first
//   Huffman decodes to lz_size bytes of LZ data; LZ then decodes to block_uncomp.
//   Canonical codes are rebuilt from code_lengths at decode time.

#include <xtl.h>

#define XBA_BLOCK_SIZE  65536

typedef enum XbaResult
{
    XBA_OK = 0,
    XBA_ERR_OPEN = 1,
    XBA_ERR_MAGIC = 2,
    XBA_ERR_READ = 3,
    XBA_ERR_CREATE = 4,
    XBA_ERR_WRITE = 5,
    XBA_ERR_CRC = 6,
    XBA_ERR_MEM = 7
} XbaResult;

const char* Xba_ResultStr(XbaResult r);

typedef void (*XbaProgressFn)(
    int   filesDone,
    int   filesTotal,
    DWORD bytesDone,
    DWORD bytesTotal);

XbaResult Xba_Extract(
    const char* xbaPath,
    const char* destFolder,
    XbaProgressFn progressFn,
    char* detailOut,
    int           detailLen);