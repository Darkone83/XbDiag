// HddBench.cpp
// XbDiag - HDD Benchmark.
//
// Measures sequential write, raw write, sequential read, cache/4K random read,
// and seek time. Results are displayed in RenderBench and exported to D:\bench.txt.
//
// Drive metadata (model, isSSD, udmaMode) is read from s_data (HddInfo.h).
// s_view is written to switch back to INFO on cleanup.

#include "HddBench.h"
#include "HddInfo.h"
#include "font.h"
#include "input.h"
#include <xtl.h>

// ============================================================================
// Benchmark defines
// ============================================================================

#define BENCH_FILE         "E:\\xbdiag_bench.tmp"
#define BENCH_FILE_SIZE    (64 * 1024 * 1024)
#define BENCH_CHUNK        (64 * 1024)
#define BENCH_SEEK_ITERS   1024
#define BENCH_CACHE_BLOCK  (512 * 1024)
#define BENCH_CACHE_PASSES 64
#define BENCH_4K_ITERS     2048
#define BENCH_4K_SIZE      4096

// ============================================================================
// BenchData
// ============================================================================

static BenchData s_bench;
static __declspec(align(512)) BYTE s_benchBuf[BENCH_CHUNK];

static DWORD BenchRand(DWORD& seed)
{
    seed = seed * 1664525UL + 1013904223UL;
    return seed;
}

static void BenchFindReadSource()
{
    // Fallback only: find largest existing file on E:\ for read-only mode.
    // Normal benchmarks always use the written temp file so conditions are symmetric.
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("E:\\*", &fd);
    DWORD  bestSize = 0;
    s_bench.readSrc[0] = '\0';

    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (fd.nFileSizeLow > bestSize)
            {
                bestSize = fd.nFileSizeLow;
                StrCopy(s_bench.readSrc, sizeof(s_bench.readSrc), "E:\\");
                StrCat2(s_bench.readSrc, sizeof(s_bench.readSrc),
                    s_bench.readSrc, fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

static void BenchStartRead()
{
    // Buffered sequential read — matches write path for apples-to-apples
    // filesystem throughput.  Both phases now measure the same stack.
    s_bench.hRead = CreateFileA(s_bench.readSrc, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (s_bench.hRead == INVALID_HANDLE_VALUE)
    {
        // Read source unreadable — skip straight to seek or done
        s_bench.readMBs = 0.f;
        s_bench.state = BENCH_SEEK;
        StrCopy(s_bench.statusMsg, sizeof(s_bench.statusMsg), "Read source unavailable");
        return;
    }
    s_bench.readTotal = 0;
    s_bench.readT0 = GetTickCount();
    s_bench.state = BENCH_READ;
}

static void BenchStart()
{
    ZeroMemory(&s_bench, sizeof(s_bench));

    // Fill write buffer with a simple pattern
    for (int i = 0; i < BENCH_CHUNK; i += 4)
    {
        s_benchBuf[i + 0] = 0xDE; s_benchBuf[i + 1] = 0xAD;
        s_benchBuf[i + 2] = 0xBE; s_benchBuf[i + 3] = 0xEF;
    }

    // Attempt write on E:\ first, fall back to D:\ (XBE dir, always writable)
    const char* writePaths[2] = { BENCH_FILE, "D:\\xbdiag_bench.tmp" };
    for (int wi = 0; wi < 2; ++wi)
    {
        s_bench.hWrite = CreateFileA(writePaths[wi], GENERIC_WRITE, 0, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        // Buffered sequential write — OS coalesces writes efficiently.
        // FlushFileBuffers() at the end commits to disk so elapsed time
        // reflects sustained write throughput, not just cache fill rate.
        if (s_bench.hWrite != INVALID_HANDLE_VALUE)
        {
            StrCopy(s_bench.readSrc, sizeof(s_bench.readSrc), writePaths[wi]);
            s_bench.tmpExists = true;
            // Preallocate: set file to full size so the timed write pass
            // overwrites a pre-laid-out extent instead of growing the file.
            // This removes FATX cluster allocation from the timed measurement.
            SetFilePointer(s_bench.hWrite, BENCH_FILE_SIZE, NULL, FILE_BEGIN);
            SetEndOfFile(s_bench.hWrite);
            SetFilePointer(s_bench.hWrite, 0, NULL, FILE_BEGIN);
            s_bench.writeTotal = 0;
            s_bench.state = BENCH_CONFIRM;
            return;
        }
    }

    // Both write paths failed — true read-only fallback
    s_bench.readOnly = true;
    s_bench.writeMBs = 0.f;
    // Capture error code for diagnosis
    DWORD writeErr = GetLastError();
    char errCode[12];
    IntToStr((int)writeErr, errCode, sizeof(errCode));

    // Try to find an existing file to benchmark reads against
    BenchFindReadSource();
    if (s_bench.readSrc[0] != '\0')
    {
        StrCopy(s_bench.statusMsg, sizeof(s_bench.statusMsg),
            "Write failed (err ");
        StrCat2(s_bench.statusMsg, sizeof(s_bench.statusMsg),
            s_bench.statusMsg, errCode);
        StrCat2(s_bench.statusMsg, sizeof(s_bench.statusMsg),
            s_bench.statusMsg, ") - READ ONLY MODE");
        BenchStartRead();
        return;
    }

    // Nothing to read either — can't benchmark
    StrCopy(s_bench.statusMsg, sizeof(s_bench.statusMsg),
        "Write failed (err ");
    StrCat2(s_bench.statusMsg, sizeof(s_bench.statusMsg),
        s_bench.statusMsg, errCode);
    StrCat2(s_bench.statusMsg, sizeof(s_bench.statusMsg),
        s_bench.statusMsg, ") - no readable files found");
    s_bench.state = BENCH_DONE;
}

static void BenchCleanup()
{
    if (s_bench.hWrite != INVALID_HANDLE_VALUE && s_bench.hWrite != NULL)
    {
        CloseHandle(s_bench.hWrite); s_bench.hWrite = INVALID_HANDLE_VALUE;
    }
    if (s_bench.hRawWr != INVALID_HANDLE_VALUE && s_bench.hRawWr != NULL)
    {
        CloseHandle(s_bench.hRawWr); s_bench.hRawWr = INVALID_HANDLE_VALUE;
    }
    if (s_bench.hRead != INVALID_HANDLE_VALUE && s_bench.hRead != NULL)
    {
        CloseHandle(s_bench.hRead); s_bench.hRead = INVALID_HANDLE_VALUE;
    }
    if (s_bench.hCache != INVALID_HANDLE_VALUE && s_bench.hCache != NULL)
    {
        CloseHandle(s_bench.hCache); s_bench.hCache = INVALID_HANDLE_VALUE;
    }
    if (s_bench.hRand4k != INVALID_HANDLE_VALUE && s_bench.hRand4k != NULL)
    {
        CloseHandle(s_bench.hRand4k); s_bench.hRand4k = INVALID_HANDLE_VALUE;
    }
    if (s_bench.hSeek != INVALID_HANDLE_VALUE && s_bench.hSeek != NULL)
    {
        CloseHandle(s_bench.hSeek); s_bench.hSeek = INVALID_HANDLE_VALUE;
    }
    if (s_bench.tmpExists)
    {
        // Delete whichever path was used (readSrc holds the write path)
        if (s_bench.readSrc[0] != '\0')
            DeleteFileA(s_bench.readSrc);
        else
            DeleteFileA(BENCH_FILE);
        s_bench.tmpExists = false;
    }
}

static void ExportBench()
{
    s_bench.exportDone = false;
    s_bench.exportOK = false;

    HANDLE hf = CreateFileA("D:\\hddbench.txt", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        s_bench.exportDone = true; return;
    }

    DWORD w;
    char  line[128];

    const char* hdr = s_data.isSSD
        ? "XbDiag HDD Benchmark (SSD)\r\n===========================\r\n"
        "FS WR:  buffered preallocated write+flush\r\n"
        "RAW WR: FILE_FLAG_NO_BUFFERING overwrite\r\n"
        "SEQ RD: FILE_FLAG_SEQUENTIAL_SCAN (buffered practical read)\r\n"
        "4K RND: 2048x 4KB random reads (IOPS)\r\n"
        "SEEK:   1024 random 512-byte reads\r\n\r\n"
        : "XbDiag HDD Benchmark (HDD)\r\n===========================\r\n"
        "FS WR:  buffered preallocated write+flush\r\n"
        "RAW WR: FILE_FLAG_NO_BUFFERING overwrite\r\n"
        "SEQ RD: FILE_FLAG_SEQUENTIAL_SCAN (buffered practical read)\r\n"
        "CACHE:  512KB x64 passes (platter buffer bandwidth)\r\n"
        "SEEK:   1024 random 512-byte reads\r\n\r\n";
    WriteFile(hf, hdr, StrLen(hdr), &w, NULL);

    // Drive identity
    StrCopy(line, sizeof(line), "Drive:      ");
    StrCat2(line, sizeof(line), line, s_data.model);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    StrCopy(line, sizeof(line), "Serial:     ");
    StrCat2(line, sizeof(line), line, s_data.serial);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    StrCopy(line, sizeof(line), "Interface:  ");
    StrCat2(line, sizeof(line), line, s_data.udmaMode);
    StrCat2(line, sizeof(line), line, "\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    StrCopy(line, sizeof(line), "Mode:       ");
    StrCat2(line, sizeof(line), line, s_bench.readOnly ? "READ ONLY (write failed)" : "Full read/write");
    StrCat2(line, sizeof(line), line, "\r\n\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    // Results — format float as "XX.X"
    auto FmtFloat = [](float v, char* out, int outLen) {
        int whole = Ftoi(v);
        float frac = v - (float)whole;
        if (frac < 0.f) frac = 0.f;
        int dec = Ftoi(frac * 10.f);
        char t[12];
        IntToStr(whole, t, sizeof(t));
        StrCopy(out, outLen, t);
        StrCat2(out, outLen, out, ".");
        IntToStr(dec, t, sizeof(t));
        StrCat2(out, outLen, out, t);
        };

    char val[16];

    if (!s_bench.readOnly)
    {
        FmtFloat(s_bench.writeMBs, val, sizeof(val));
        StrCopy(line, sizeof(line), "FS Write:    ");
        StrCat2(line, sizeof(line), line, val);
        StrCat2(line, sizeof(line), line, " MB/s\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);

        FmtFloat(s_bench.rawWrMBs, val, sizeof(val));
        StrCopy(line, sizeof(line), "Raw Write:   ");
        StrCat2(line, sizeof(line), line, val);
        StrCat2(line, sizeof(line), line, " MB/s\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);
    }

    FmtFloat(s_bench.readMBs, val, sizeof(val));
    StrCopy(line, sizeof(line), "Seq Read:    ");
    StrCat2(line, sizeof(line), line, val);
    StrCat2(line, sizeof(line), line, " MB/s\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    if (s_data.isSSD)
    {
        char iopsEx[12]; IntToStr(Ftoi(s_bench.rand4kIOPS), iopsEx, sizeof(iopsEx));
        StrCopy(line, sizeof(line), "4K Random:   ");
        StrCat2(line, sizeof(line), line, iopsEx);
        StrCat2(line, sizeof(line), line, " IOPS\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);
    }
    else
    {
        FmtFloat(s_bench.cacheMBs, val, sizeof(val));
        StrCopy(line, sizeof(line), "Buf Read:    ");
        StrCat2(line, sizeof(line), line, val);
        StrCat2(line, sizeof(line), line, " MB/s\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);
    }

    FmtFloat(s_bench.seekMs, val, sizeof(val));
    StrCopy(line, sizeof(line), "Seek Time:   ");
    StrCat2(line, sizeof(line), line, val);
    StrCat2(line, sizeof(line), line, " ms avg\r\n");
    WriteFile(hf, line, StrLen(line), &w, NULL);

    if (s_bench.readOnly && s_bench.statusMsg[0])
    {
        StrCopy(line, sizeof(line), "Note:        ");
        StrCat2(line, sizeof(line), line, s_bench.statusMsg);
        StrCat2(line, sizeof(line), line, "\r\n");
        WriteFile(hf, line, StrLen(line), &w, NULL);
    }

    FlushFileBuffers(hf);
    CloseHandle(hf);
    s_bench.exportDone = true;
    s_bench.exportOK = true;
}

// BenchTick — called every frame, but IO runs in a tight inner loop.
//
// The old design did one 64KB chunk per frame, then rendered.  At 60fps that
// caps throughput at 64KB * 60 = 3.84 MB/s regardless of drive speed.
//
// Fix: loop IO for ~200ms per call (5fps render rate), yielding for a render
// only when nextRender is due.  The measured elapsed time comes from
// GetTickCount() bracketing the tight IO loop, so vsync never enters the
// denominator.  Seek latency is unaffected — each seek is its own operation
// and timing is per-seek, not per-frame.
static void BenchTick()
{
    switch (s_bench.state)
    {
    case BENCH_CONFIRM:
        // Waiting for user to press [A] — handled in input, not here
        break;

    case BENCH_WRITE:
    {
        // Tight write loop — run until file is done, no per-frame yield.
        // Timer brackets only the WriteFile calls so render overhead is excluded.
        while (s_bench.writeTotal < (DWORD)BENCH_FILE_SIZE)
        {
            DWORD remaining = (DWORD)BENCH_FILE_SIZE - s_bench.writeTotal;
            DWORD toWrite = (remaining < BENCH_CHUNK) ? remaining : BENCH_CHUNK;

            DWORD written = 0;
            WriteFile(s_bench.hWrite, s_benchBuf, toWrite, &written, NULL);
            s_bench.writeTotal += written;
            if (written == 0) break;  // write error
        }

        // Flush to disk before timing stops — measures true write throughput
        FlushFileBuffers(s_bench.hWrite);
        CloseHandle(s_bench.hWrite);
        s_bench.hWrite = INVALID_HANDLE_VALUE;

        DWORD elapsed = GetTickCount() - s_bench.writeT0;
        if (elapsed > 0)
            s_bench.writeMBs = (float)s_bench.writeTotal / 1048576.f
            / ((float)elapsed / 1000.f);

        // Open same preallocated file for unbuffered overwrite
        s_bench.hRawWr = CreateFileA(s_bench.readSrc, GENERIC_WRITE, 0,
            NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
        if (s_bench.hRawWr == INVALID_HANDLE_VALUE)
        {
            s_bench.rawWrMBs = 0.f; BenchStartRead();
        }
        else
        {
            s_bench.rawWrTotal = 0;
            s_bench.rawWrT0 = GetTickCount();
            s_bench.state = BENCH_RAW_WR;
        }
        break;
    }

    case BENCH_RAW_WR:
    {
        // Unbuffered overwrite — 512-byte aligned buffer, no OS cache
        while (s_bench.rawWrTotal < (DWORD)BENCH_FILE_SIZE)
        {
            DWORD rem = (DWORD)BENCH_FILE_SIZE - s_bench.rawWrTotal;
            DWORD nw = 0;
            WriteFile(s_bench.hRawWr, s_benchBuf,
                rem < BENCH_CHUNK ? rem : BENCH_CHUNK, &nw, NULL);
            s_bench.rawWrTotal += nw;
            if (nw == 0) break;
        }
        CloseHandle(s_bench.hRawWr); s_bench.hRawWr = INVALID_HANDLE_VALUE;
        DWORD rawEl = GetTickCount() - s_bench.rawWrT0;
        if (rawEl > 0 && s_bench.rawWrTotal > 0)
            s_bench.rawWrMBs = (float)s_bench.rawWrTotal / 1048576.f
            / ((float)rawEl / 1000.f);
        BenchStartRead();
        break;
    }

    case BENCH_READ:
    {
        // Tight read loop — same approach as write.
        while (s_bench.readTotal < (DWORD)BENCH_FILE_SIZE)
        {
            DWORD bytesRead = 0;
            ReadFile(s_bench.hRead, s_benchBuf, BENCH_CHUNK, &bytesRead, NULL);
            s_bench.readTotal += bytesRead;
            if (bytesRead == 0) break;  // EOF or error
        }

        CloseHandle(s_bench.hRead);
        s_bench.hRead = INVALID_HANDLE_VALUE;

        DWORD elapsed = GetTickCount() - s_bench.readT0;
        if (elapsed > 0 && s_bench.readTotal > 0)
            s_bench.readMBs = (float)s_bench.readTotal / 1048576.f
            / ((float)elapsed / 1000.f);

        if (s_data.isSSD)
        {
            s_bench.hRand4k = CreateFileA(s_bench.readSrc, GENERIC_READ,
                FILE_SHARE_READ, NULL, OPEN_EXISTING,
                FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS, NULL);
            if (s_bench.hRand4k == INVALID_HANDLE_VALUE)
            {
                s_bench.rand4kMBs = 0.f; s_bench.rand4kIOPS = 0.f;
                s_bench.state = BENCH_SEEK;
            }
            else
            {
                s_bench.rand4kIdx = 0;
                s_bench.rand4kT0 = GetTickCount();
                s_bench.state = BENCH_4K_RAND;
            }
        }
        else
        {
            s_bench.hCache = CreateFileA(s_bench.readSrc, GENERIC_READ,
                FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
            if (s_bench.hCache == INVALID_HANDLE_VALUE)
            {
                s_bench.cacheMBs = 0.f; s_bench.state = BENCH_SEEK;
            }
            else
            {
                s_bench.cachePass = 0;
                s_bench.cacheTotal = 0;
                s_bench.cacheT0 = GetTickCount();
                s_bench.state = BENCH_CACHE_RD;
            }
        }
        break;
    }

    case BENCH_CACHE_RD:
    {
        // 512KB block repeated 64 times (32MB). Drive serves subsequent passes
        // from its internal read-ahead buffer — measures drive buffer bandwidth.
        while (s_bench.cachePass < BENCH_CACHE_PASSES)
        {
            SetFilePointer(s_bench.hCache, 0, NULL, FILE_BEGIN);
            DWORD passBytes = 0;
            while (passBytes < (DWORD)BENCH_CACHE_BLOCK)
            {
                DWORD toRead = BENCH_CHUNK;
                if (toRead > (DWORD)BENCH_CACHE_BLOCK - passBytes)
                    toRead = (DWORD)BENCH_CACHE_BLOCK - passBytes;
                DWORD nr = 0;
                ReadFile(s_bench.hCache, s_benchBuf, toRead, &nr, NULL);
                if (nr == 0) break;
                passBytes += nr;
                s_bench.cacheTotal += nr;
            }
            s_bench.cachePass++;
        }
        CloseHandle(s_bench.hCache); s_bench.hCache = INVALID_HANDLE_VALUE;
        DWORD cEl = GetTickCount() - s_bench.cacheT0;
        if (cEl > 0 && s_bench.cacheTotal > 0)
            s_bench.cacheMBs = (float)s_bench.cacheTotal / 1048576.f
            / ((float)cEl / 1000.f);

        // Open seek handle
        s_bench.hSeek = CreateFileA(s_bench.readSrc, GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS, NULL);
        if (s_bench.hSeek == INVALID_HANDLE_VALUE)
        {
            s_bench.seekMs = 0.f; s_bench.state = BENCH_DONE;
            if (s_bench.tmpExists)
            {
                DeleteFileA(s_bench.readSrc[0] ? s_bench.readSrc : BENCH_FILE);
                s_bench.tmpExists = false;
            }
        }
        else
        {
            s_bench.seekIdx = 0;
            s_bench.seekT0 = GetTickCount();
            s_bench.state = BENCH_SEEK;
        }
        break;
    }

    case BENCH_4K_RAND:
    {
        // SSD 4K random read test — time-sliced 30ms per frame.
        // Reports IOPS and MB/s.
        static DWORD seed4k = 0xCAFEBABE;
        DWORD fSize4k = GetFileSize(s_bench.hRand4k, NULL);
        if (fSize4k < BENCH_4K_SIZE) fSize4k = BENCH_4K_SIZE;
        DWORD range4k = (fSize4k / BENCH_4K_SIZE) - 1;

        DWORD sl4k = GetTickCount();
        while (s_bench.rand4kIdx < BENCH_4K_ITERS)
        {
            DWORD page = range4k > 0 ? (BenchRand(seed4k) % range4k) : 0;
            SetFilePointer(s_bench.hRand4k, (LONG)(page * BENCH_4K_SIZE), NULL, FILE_BEGIN);
            DWORD nr = 0;
            ReadFile(s_bench.hRand4k, s_benchBuf, BENCH_4K_SIZE, &nr, NULL);
            s_bench.rand4kIdx++;
            if ((GetTickCount() - sl4k) >= 30) break;
        }

        if (s_bench.rand4kIdx >= BENCH_4K_ITERS)
        {
            CloseHandle(s_bench.hRand4k); s_bench.hRand4k = INVALID_HANDLE_VALUE;
            DWORD el4k = GetTickCount() - s_bench.rand4kT0;
            if (el4k > 0)
            {
                float secs4k = (float)el4k / 1000.f;
                s_bench.rand4kIOPS = (float)BENCH_4K_ITERS / secs4k;
                s_bench.rand4kMBs = s_bench.rand4kIOPS * BENCH_4K_SIZE / 1048576.f;
            }
            s_bench.hSeek = CreateFileA(s_bench.readSrc, GENERIC_READ,
                FILE_SHARE_READ, NULL, OPEN_EXISTING,
                FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS, NULL);
            if (s_bench.hSeek == INVALID_HANDLE_VALUE)
            {
                s_bench.seekMs = 0.f; s_bench.state = BENCH_DONE;
                if (s_bench.tmpExists)
                {
                    DeleteFileA(s_bench.readSrc[0] ? s_bench.readSrc : BENCH_FILE);
                    s_bench.tmpExists = false;
                }
            }
            else
            {
                s_bench.seekIdx = 0;
                s_bench.seekT0 = GetTickCount();
                s_bench.state = BENCH_SEEK;
            }
        }
        break;
    }

    case BENCH_SEEK:
    {
        // Seek runs one operation at a time so latency per-seek is meaningful.
        // Do a batch per frame but not a tight loop — each seek+read is ~10ms
        // so 16 per frame at 60fps = ~160ms/frame which would block rendering.
        // Use a time-sliced approach: run seeks for up to 30ms then yield.
        static DWORD seekSeed = 0xDEADBEEF;

        DWORD fileSize = GetFileSize(s_bench.hSeek, NULL);
        if (fileSize < 512) fileSize = 512;
        DWORD range = (fileSize / 512) - 1;

        DWORD sliceStart = GetTickCount();
        while (s_bench.seekIdx < BENCH_SEEK_ITERS)
        {
            DWORD sector = range > 0 ? (BenchRand(seekSeed) % range) : 0;
            DWORD offset = sector * 512;
            SetFilePointer(s_bench.hSeek, (LONG)offset, NULL, FILE_BEGIN);
            DWORD bytesRead = 0;
            ReadFile(s_bench.hSeek, s_benchBuf, 512, &bytesRead, NULL);
            s_bench.seekIdx++;
            // Yield for render every ~30ms so the progress display updates
            if ((GetTickCount() - sliceStart) >= 30) break;
        }

        if (s_bench.seekIdx >= BENCH_SEEK_ITERS)
        {
            CloseHandle(s_bench.hSeek);
            s_bench.hSeek = INVALID_HANDLE_VALUE;

            DWORD elapsed = GetTickCount() - s_bench.seekT0;
            if (BENCH_SEEK_ITERS > 0)
                s_bench.seekMs = (float)elapsed / (float)BENCH_SEEK_ITERS;

            if (s_bench.tmpExists)
            {
                DeleteFileA(s_bench.readSrc[0] ? s_bench.readSrc : BENCH_FILE);
                s_bench.tmpExists = false;
            }

            s_bench.state = BENCH_DONE;
        }
        break;
    }

    default:
        break;
    }
}

// Render benchmark bar (value 0..maxVal mapped to barW pixels)
static void DrawBenchBar(float x, float y, float barW, float val, float maxVal, DWORD col)
{
    float frac = val / maxVal;
    if (frac > 1.f) frac = 1.f;
    if (frac < 0.f) frac = 0.f;
    const float BH = 7.f;
    FillRect(x, y + 2.f, x + barW, y + 2.f + BH, D3DCOLOR_XRGB(20, 25, 40));
    DWORD dimR = ((col >> 16) & 0xFF) >> 1;
    DWORD dimG = ((col >> 8) & 0xFF) >> 1;
    DWORD dimB = ((col) & 0xFF) >> 1;
    FillRectGrad(x, y + 2.f, x + barW * frac, y + 2.f + BH, col,
        D3DCOLOR_XRGB(dimR, dimG, dimB));
    HLine(y + 2.f, x, x + barW, COL_BORDER);
    HLine(y + 2.f + BH, x, x + barW, COL_BORDER);
}

static void RenderBench(const DiagLogo& logo)
{
    g_pDevice->BeginScene();

    const char* hint;
    if (s_bench.state == BENCH_IDLE)
        hint = "[A] Start Benchmark    [Left] Drive Info    [B] Back";
    else if (s_bench.state == BENCH_DONE)
    {
        if (s_bench.exportDone)
            hint = s_bench.exportOK
            ? "[A] Saved OK    [Left] Drive Info    [B] Back"
            : "[A] Save failed [Left] Drive Info    [B] Back";
        else
            hint = "[A] Save hddbench.txt    [Left] Drive Info    [B] Back";
    }
    else
        if (s_bench.state == BENCH_CONFIRM)
            hint = "[A] Confirm start    [B] Cancel";
        else
            hint = "Benchmarking...    [B] Cancel";

    DrawPageChrome(logo, "HDD BENCHMARK", hint);

    float y = CONTENT_Y + 8.f;
    const float VX = LM + 140.f;
    const float BX = VX + 80.f;
    const float BW = 180.f;
    const float RLH = LINE_H + 2.f;

    // Drive info strip
    DrawText(LM, y, "DRIVE :", 1.2f, COL_GRAY);
    DrawText(VX, y, s_data.model, 1.2f, COL_CYAN);
    y += RLH;
    DrawText(LM, y, "IFACE :", 1.2f, COL_GRAY);
    DrawText(VX, y, s_data.udmaMode, 1.2f, COL_WHITE);
    // * = drive capability only; host active mode not confirmed by controller
    if (s_data.udmaMode[0] && s_data.udmaMode[StrLen(s_data.udmaMode) - 1] == '*')
        DrawText(VX + TW(s_data.udmaMode, 1.2f) + 4.f, y,
            "(drive cap, host mode unconfirmed)", 1.0f, COL_ORANGE);
    y += RLH + 4.f;
    HLine(y, LM, SW - LM, COL_BORDER);
    y += 6.f;

    // Read-only notice
    if (s_bench.readOnly)
    {
        DrawText(LM, y, "READ ONLY MODE", 1.2f, COL_YELLOW);
        y += RLH;
        DrawText(LM, y, s_bench.statusMsg, 1.1f, D3DCOLOR_XRGB(180, 140, 60));
        y += RLH + 4.f;
    }

    // Results
    auto FmtFloat = [](float v, char* out, int outLen) {
        int whole = Ftoi(v);
        float frac = v - (float)whole;
        if (frac < 0.f) frac = 0.f;
        int dec = Ftoi(frac * 10.f);
        char t[12];
        IntToStr(whole, t, sizeof(t));
        StrCopy(out, outLen, t);
        StrCat2(out, outLen, out, ".");
        IntToStr(dec, t, sizeof(t));
        StrCat2(out, outLen, out, t);
        };

    char valStr[20];

    // WRITE
    if (!s_bench.readOnly)
    {
        DrawText(LM, y, "FS WR :", 1.2f, COL_GRAY);
        if (s_bench.state == BENCH_WRITE)
        {
            // Progress during write
            DWORD pct = s_bench.writeTotal * 100 / BENCH_FILE_SIZE;
            char prog[12]; IntToStr((int)pct, prog, sizeof(prog));
            StrCat2(prog, sizeof(prog), prog, "%");
            DrawText(VX, y, prog, 1.2f, COL_YELLOW);
        }
        else if (s_bench.writeMBs > 0.f || s_bench.state > BENCH_WRITE)
        {
            FmtFloat(s_bench.writeMBs, valStr, sizeof(valStr));
            StrCat2(valStr, sizeof(valStr), valStr, " MB/s");
            DrawText(VX, y, valStr, 1.2f, COL_GREEN);
            DrawBenchBar(BX, y, BW, s_bench.writeMBs, 100.f, COL_GREEN);
        }
        else
            DrawText(VX, y, "---", 1.2f, COL_DIM);
        y += RLH;
    }

    // RAW WRITE
    if (!s_bench.readOnly)
    {
        DrawText(LM, y, "RAW WR:", 1.2f, COL_GRAY);
        if (s_bench.state == BENCH_RAW_WR)
        {
            DWORD pct = s_bench.rawWrTotal * 100 / BENCH_FILE_SIZE;
            char prog[12]; IntToStr((int)pct, prog, sizeof(prog));
            StrCat2(prog, sizeof(prog), prog, "%");
            DrawText(VX, y, prog, 1.2f, COL_YELLOW);
        }
        else if (s_bench.rawWrMBs > 0.f || (int)s_bench.state > (int)BENCH_RAW_WR)
        {
            FmtFloat(s_bench.rawWrMBs, valStr, sizeof(valStr));
            StrCat2(valStr, sizeof(valStr), valStr, " MB/s");
            DrawText(VX, y, valStr, 1.2f, COL_GREEN);
            DrawBenchBar(BX, y, BW, s_bench.rawWrMBs, 100.f, COL_GREEN);
        }
        else
            DrawText(VX, y, "---", 1.2f, COL_DIM);
        y += RLH;
    }

    // SEQ READ
    DrawText(LM, y, "SEQ RD:", 1.2f, COL_GRAY);
    if (s_bench.state == BENCH_READ)
    {
        DWORD readDenom = (s_bench.hRead && s_bench.hRead != INVALID_HANDLE_VALUE)
            ? GetFileSize(s_bench.hRead, NULL) : (DWORD)BENCH_FILE_SIZE;
        if (readDenom == INVALID_FILE_SIZE || readDenom == 0 ||
            readDenom > (DWORD)BENCH_FILE_SIZE)
            readDenom = (DWORD)BENCH_FILE_SIZE;
        DWORD pct = readDenom > 0 ? s_bench.readTotal * 100 / readDenom : 0;
        char prog[12]; IntToStr((int)pct, prog, sizeof(prog));
        StrCat2(prog, sizeof(prog), prog, "%");
        DrawText(VX, y, prog, 1.2f, COL_YELLOW);
    }
    else if (s_bench.readMBs > 0.f)
    {
        FmtFloat(s_bench.readMBs, valStr, sizeof(valStr));
        StrCat2(valStr, sizeof(valStr), valStr, " MB/s");
        DrawText(VX, y, valStr, 1.2f, COL_CYAN);
        DrawBenchBar(BX, y, BW, s_bench.readMBs, 120.f, COL_CYAN);
    }
    else
        DrawText(VX, y, "---", 1.2f, COL_DIM);
    y += RLH;

    // CACHE (HDD) / 4K RAND (SSD)
    if (s_data.isSSD)
    {
        DrawText(LM, y, "4K RND:", 1.2f, COL_GRAY);
        if (s_bench.state == BENCH_4K_RAND)
        {
            char p4k[16]; IntToStr(s_bench.rand4kIdx, p4k, sizeof(p4k));
            StrCat2(p4k, sizeof(p4k), p4k, "/2048");
            DrawText(VX, y, p4k, 1.2f, COL_YELLOW);
        }
        else if (s_bench.rand4kIOPS > 0.f)
        {
            char iopsStr[12], mbStr[12], result4k[40];
            IntToStr(Ftoi(s_bench.rand4kIOPS), iopsStr, sizeof(iopsStr));
            FmtFloat(s_bench.rand4kMBs, mbStr, sizeof(mbStr));
            StrCopy(result4k, sizeof(result4k), iopsStr);
            StrCat2(result4k, sizeof(result4k), result4k, " IOPS  (");
            StrCat2(result4k, sizeof(result4k), result4k, mbStr);
            StrCat2(result4k, sizeof(result4k), result4k, " MB/s)");
            DrawText(VX, y, result4k, 1.2f, COL_CYAN);
            DrawBenchBar(BX + 80.f, y, BW - 80.f, s_bench.rand4kIOPS, 50000.f, COL_CYAN);
        }
        else
            DrawText(VX, y, "---", 1.2f, COL_DIM);
    }
    else
    {
        DrawText(LM, y, "BUF RD:", 1.2f, COL_GRAY);
        if (s_bench.state == BENCH_CACHE_RD)
        {
            char cprog[16]; IntToStr(s_bench.cachePass, cprog, sizeof(cprog));
            StrCat2(cprog, sizeof(cprog), cprog, "/64");
            DrawText(VX, y, cprog, 1.2f, COL_YELLOW);
        }
        else if (s_bench.cacheMBs > 0.f)
        {
            FmtFloat(s_bench.cacheMBs, valStr, sizeof(valStr));
            StrCat2(valStr, sizeof(valStr), valStr, " MB/s");
            DrawText(VX, y, valStr, 1.2f, COL_CYAN);
            DrawBenchBar(BX, y, BW, s_bench.cacheMBs, 200.f, COL_CYAN);
        }
        else
            DrawText(VX, y, "---", 1.2f, COL_DIM);
    }
    y += RLH;

    // SEEK
    DrawText(LM, y, "SEEK  :", 1.2f, COL_GRAY);
    if (s_bench.state == BENCH_SEEK)
    {
        char prog[16];
        IntToStr(s_bench.seekIdx, prog, sizeof(prog));
        StrCat2(prog, sizeof(prog), prog, "/1024");
        DrawText(VX, y, prog, 1.2f, COL_YELLOW);
    }
    else if (s_bench.seekMs > 0.f)
    {
        FmtFloat(s_bench.seekMs, valStr, sizeof(valStr));
        StrCat2(valStr, sizeof(valStr), valStr, " ms avg");
        DWORD seekCol = (s_bench.seekMs < 5.f) ? COL_GREEN
            : (s_bench.seekMs < 15.f) ? COL_CYAN : COL_YELLOW;
        DrawText(VX, y, valStr, 1.2f, seekCol);
        // Seek bar inverted: lower is better, max display 30ms
        float seekFrac = s_bench.seekMs / 30.f;
        if (seekFrac > 1.f) seekFrac = 1.f;
        DrawBenchBar(BX, y, BW, seekFrac, 1.f, seekCol);
    }
    else
        DrawText(VX, y, "---", 1.2f, COL_DIM);
    y += RLH;

    // Read source (fallback)
    if (s_bench.readOnly && s_bench.readSrc[0])
    {
        y += 4.f;
        DrawText(LM, y, "SOURCE:", 1.2f, COL_GRAY);
        DrawText(VX, y, s_bench.readSrc, 1.1f, D3DCOLOR_XRGB(160, 160, 160));
        y += RLH;
    }

    // Idle prompt
    if (s_bench.state == BENCH_IDLE)
    {
        y += 8.f;
        DrawText(LM, y, "Press [A] to start benchmark", 1.2f, COL_GRAY);
        y += LINE_H;
        DrawText(LM, y, "64MB write + read + 1024 seek ops", 1.1f, COL_DIM);
        DrawText(LM, y + LINE_H, "Buffered filesystem I/O  --  practical throughput", 1.1f, COL_DIM);
    }

    // Confirm overlay
    if (s_bench.state == BENCH_CONFIRM)
    {
        const float CW = 360.f;
        const float CH = 80.f;
        const float CX = (SW - CW) * 0.5f;
        const float CY = SH * 0.5f - CH * 0.5f;
        FillRectGrad(CX, CY, CX + CW, CY + CH,
            D3DCOLOR_XRGB(20, 28, 70), D3DCOLOR_XRGB(12, 16, 46));
        HLine(CY, CX, CX + CW, COL_CYAN);
        HLine(CY + CH, CX, CX + CW, COL_CYAN);
        VLine(CX, CY, CY + CH, COL_BORDER);
        VLine(CX + CW, CY, CY + CH, COL_BORDER);
        DrawText(CX + 12.f, CY + 8.f, "START HDD BENCHMARK?", 1.25f, COL_WHITE);
        DrawText(CX + 12.f, CY + 26.f, "64MB buffered write + read + 1024 seek ops", 1.1f, COL_YELLOW);
        DrawText(CX + 12.f, CY + 40.f, "Screen freezes during test.  ~30-60s.", 1.1f, COL_GRAY);
        DrawText(CX + 12.f, CY + 56.f, "[A] Confirm    [B] Cancel", 1.1f, COL_CYAN);
    }

    // Active phase overlay — shows while screen is frozen during IO
    if (s_bench.state == BENCH_WRITE || s_bench.state == BENCH_READ)
    {
        const char* phaseStr = (s_bench.state == BENCH_WRITE)
            ? "WRITING 64MB...  please wait"
            : "READING 64MB...  please wait";
        float ty = SH * 0.5f - 8.f;
        float tw = TW(phaseStr, 1.3f);
        DrawText((SW - tw) * 0.5f, ty, phaseStr, 1.3f, COL_YELLOW);
    }

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ============================================================================
// Public API wrappers
// ============================================================================

void HddBench_Start() { BenchStart(); }
void HddBench_Cleanup() { BenchCleanup(); }
void HddBench_Tick() { BenchTick(); }
void HddBench_Export() { ExportBench(); }

void HddBench_Render(const DiagLogo& logo)
{
    RenderBench(logo);
}

bool HddBench_IsRunning()
{
    return (s_bench.state != BENCH_IDLE &&
        s_bench.state != BENCH_CONFIRM &&
        s_bench.state != BENCH_DONE);
}

// AutoRun access — called by HddInfo_AutoRun
BenchData& HddBench_GetData() { return s_bench; }

void HddBench_Reset()
{
    BenchCleanup();
    ZeroMemory(&s_bench, sizeof(s_bench));
}