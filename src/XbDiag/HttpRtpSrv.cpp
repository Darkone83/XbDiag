// HttpRptSrv.cpp
// XbDiag - HTTP Report Server
//
// Background service. No UI, no module state machine.
// Hooks in main.cpp identically to FtpServ:
//   HttpRptSrv_Start() -- once, after Update_StartBootCheck()
//   HttpRptSrv_Poll()  -- every frame, alongside FtpServ_Tick()
//
// Pages:
//   GET  /sysinfo   -- hardware snapshot (auto-refresh every 30s)
//   GET  /report    -- report file viewer with auto-detect + download links
//   GET  /settings  -- XbSet editor (GET=view, POST=save)
//   GET  /about     -- about page with branding
//   GET  /download  -- binary file download streamed in chunks
//   POST /settings  -- save XbDiag.set
//
// RXDK constraints:
//   No sprintf/sscanf/strlen -- DiagCommon helpers throughout
//   No lambdas, no std::string
//   File-scope statics only
//   MSVC 2003 / C89 declaration ordering

#include "HttpRptSrv.h"
#include "SysInfo.h"
#include "Update.h"
#include "XbSet.h"
#include "StressTest.h"
#include "DiagCommon.h"
#include <xtl.h>
#include <winsockx.h>

// Stress state externs -- same pattern as XbSet.cpp
extern StressState s_state;      // CPU stress state
extern StressState s_ramState;   // RAM stress state
extern StressState s_gpuState;   // GPU stress state
extern BYTE  StressAutoRun_GetMinCPU();
extern BYTE  StressAutoRun_GetMaxCPU();
extern BYTE  StressAutoRun_GetMinFan();
extern BYTE  StressAutoRun_GetMaxFan();
extern BYTE  StressAutoRun_GetMeasuredLoad();
extern bool  StressAutoRun_GetThermalAbort();
extern DWORD RamAutoRun_GetSweeps();
extern DWORD RamAutoRun_GetErrors();
extern int   RamAutoRun_GetFailed();
extern DWORD GpuAutoRun_GetLoops();
extern float GpuAutoRun_GetPeakFPS();
extern float GpuAutoRun_GetMinFPS();

// ============================================================================
// Report / export file table
// ============================================================================

struct ReportFile
{
    const char* filename;
    const char* desc;
    bool        isBinary;
};

static const ReportFile k_files[] =
{
    { "XbDiag.txt",    "XbSet automated diagnostic report",       false },
    { "sysinfo.txt",   "System Info hardware snapshot",           false },
    { "hddinfo.txt",   "HDD identity and benchmark results",      false },
    { "smart.txt",     "HDD SMART attributes",                    false },
    { "hddbench.txt",  "HDD sequential read benchmark",           false },
    { "ramresult.csv", "Memory test per-bank results",            false },
    { "bios.bin",      "BIOS flash dump",                         true  },
    { "eeprom.bin",    "EEPROM raw 256-byte backup",              true  },
};
static const int k_fileCount = sizeof(k_files) / sizeof(k_files[0]);

// ============================================================================
// Internal state
// ============================================================================

static SOCKET s_listenSock = INVALID_SOCKET;
static bool   s_started = false;
static bool   s_bound = false;
static char   s_localIP[20];

// Last save result for settings page feedback
static bool   s_saveAttempted = false;
static bool   s_saveOK = false;

// ============================================================================
// HTML response buffer -- 128KB for About page with embedded images
// ============================================================================

static const int k_bufSize = 131072;
static char      s_buf[k_bufSize];
static int       s_bufLen = 0;

static const int k_fileReadSize = 32768;
static char      s_fileReadBuf[k_fileReadSize];

// POST body buffer
// Image data forward declarations -- defined in image data section
static const char* k_img_xbdiag = NULL;
static const char* k_img_tr = NULL;
static const char* k_img_d83 = NULL;
static const char* k_img_dc = NULL;

// ============================================================================
// Buffer helpers
// ============================================================================

static void BA(const char* s)
{
    int i;
    for (i = 0; s[i] && s_bufLen < k_bufSize - 1; ++i)
        s_buf[s_bufLen++] = s[i];
}

static void BAE(const char* s)
{
    int i;
    if (!s) return;
    for (i = 0; s[i]; ++i)
    {
        if (s[i] == '&') BA("&amp;");
        else if (s[i] == '<') BA("&lt;");
        else if (s[i] == '>') BA("&gt;");
        else if (s[i] == '"') BA("&quot;");
        else { if (s_bufLen < k_bufSize - 1) s_buf[s_bufLen++] = s[i]; }
    }
}

static void BAI(int v) { char t[16]; IntToStr(v, t, sizeof(t)); BA(t); }
static void BAB(bool v) { BA(v ? "1" : "0"); }

// ============================================================================
// String helpers
// ============================================================================

static bool SEq(const char* a, const char* b)
{
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == '\0' && *b == '\0';
}

static bool SEnd(const char* s, const char* suf)
{
    int sl = StrLen(s), ul = StrLen(suf);
    if (ul > sl) return false;
    return SEq(s + sl - ul, suf);
}

// Extract query/POST param value
static bool GetParam(const char* src, const char* key,
    char* out, int outLen)
{
    int kl = StrLen(key);
    int i = 0;
    const char* p = src;
    out[0] = '\0';
    while (*p)
    {
        bool m = true;
        int ki;
        for (ki = 0; ki < kl; ki++) if (p[ki] != key[ki]) { m = false; break; }
        if (m && p[kl] == '=')
        {
            p += kl + 1;
            i = 0;
            while (*p && *p != '&' && i < outLen - 1)
            {
                // URL decode + -> space, %XX -> char
                if (*p == '+') { out[i++] = ' '; ++p; }
                else if (*p == '%' && p[1] && p[2])
                {
                    char hi = p[1], lo = p[2];
                    int h = (hi >= 'A') ? (hi - 'A' + 10) : (hi >= 'a') ? (hi - 'a' + 10) : (hi - '0');
                    int l = (lo >= 'A') ? (lo - 'A' + 10) : (lo >= 'a') ? (lo - 'a' + 10) : (lo - '0');
                    out[i++] = (char)((h << 4) | l);
                    p += 3;
                }
                else out[i++] = *p++;
            }
            out[i] = '\0';
            return true;
        }
        while (*p && *p != '&')++p;
        if (*p == '&')++p;
    }
    return false;
}

static bool ParamBool(const char* src, const char* key)
{
    char v[4]; GetParam(src, key, v, sizeof(v));
    return SEq(v, "1") || SEq(v, "on");
}

static int ParamInt(const char* src, const char* key, int lo, int hi, int def)
{
    char v[8]; if (!GetParam(src, key, v, sizeof(v))) return def;
    int r = 0;
    int i;
    for (i = 0; v[i] >= '0' && v[i] <= '9'; ++i) r = r * 10 + (v[i] - '0');
    return (r >= lo && r <= hi) ? r : def;
}

// ============================================================================
// CSS
// ============================================================================

static void EmitCSS()
{
    BA("<!DOCTYPE html><html lang=en><head>"
        "<meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<link rel=preconnect href='https://fonts.googleapis.com'>"
        "<link rel=preconnect href='https://fonts.gstatic.com' crossorigin>"
        "<link href='https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&family=JetBrains+Mono:wght@400;500&display=swap' rel=stylesheet>"
        "<style>"

        /* ── Reset ─────────────────────────────────────────────── */
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{background:#06080F;color:#B8C0D0;"
        "font-family:'Inter',system-ui,sans-serif;font-size:14px;line-height:1.5;"
        "min-height:100vh}"
        "a{color:#50DCFF;text-decoration:none;transition:color .2s}"
        "a:hover{color:#A0EEFF}"
        "code,.mono{font-family:'JetBrains Mono',monospace}"

        /* ── Animated background grid ──────────────────────────── */
        "body::before{"
        "content:'';"
        "position:fixed;inset:0;z-index:0;"
        "background-image:"
        "linear-gradient(rgba(80,220,255,.03) 1px,transparent 1px),"
        "linear-gradient(90deg,rgba(80,220,255,.03) 1px,transparent 1px);"
        "background-size:40px 40px;"
        "pointer-events:none}"

        /* ── Corner bracket decoration ──────────────────────────── */
        ".bracket{"
        "position:relative}"
        ".bracket::before,.bracket::after{"
        "content:'';"
        "position:absolute;"
        "width:12px;height:12px;"
        "border-color:#50DCFF;"
        "border-style:solid;"
        "opacity:.4;"
        "transition:opacity .2s}"
        ".bracket:hover::before,.bracket:hover::after{opacity:.8}"
        ".bracket::before{"
        "top:-1px;left:-1px;"
        "border-width:1px 0 0 1px}"
        ".bracket::after{"
        "top:-1px;right:-1px;"
        "border-width:1px 1px 0 0}"

        /* ── Header ─────────────────────────────────────────────── */
        "#hdr{"
        "position:relative;z-index:10;"
        "background:linear-gradient(180deg,#0A1020 0%,#060A18 100%);"
        "border-bottom:1px solid rgba(80,220,255,.12);"
        "padding:0 28px;"
        "display:flex;align-items:stretch;gap:0;"
        "box-shadow:0 4px 32px rgba(0,0,0,.6),0 1px 0 rgba(80,220,255,.08)}"
        "#hdr-inner{"
        "display:flex;align-items:center;gap:16px;padding:14px 0;flex:1}"

        /* Logo block with animated pulse ring */
        ".logoblock{"
        "position:relative;flex-shrink:0}"
        ".logomark{"
        "width:40px;height:40px;"
        "background:linear-gradient(135deg,#1A4060,#0D2840);"
        "border:1px solid rgba(80,220,255,.3);"
        "border-radius:8px;"
        "display:flex;align-items:center;justify-content:center;"
        "font-weight:700;font-size:14px;color:#50DCFF;"
        "letter-spacing:-1px;"
        "box-shadow:0 0 20px rgba(80,220,255,.15),inset 0 1px 0 rgba(255,255,255,.05)}"
        ".logoring{"
        "position:absolute;inset:-4px;"
        "border:1px solid rgba(80,220,255,.2);"
        "border-radius:12px;"
        "animation:pulse 3s ease-in-out infinite}"
        "@keyframes pulse{"
        "0%,100%{opacity:.2;transform:scale(1)}"
        "50%{opacity:.6;transform:scale(1.04)}}"

        ".brand{font-size:20px;font-weight:700;color:#E8F0FF;letter-spacing:.5px;"
        "text-shadow:0 0 30px rgba(80,220,255,.2)}"
        ".sub{font-size:11px;color:#3A4860;margin-top:2px;letter-spacing:.2px}"

        /* Status dot in header */
        ".hdr-status{"
        "margin-left:auto;display:flex;align-items:center;gap:8px;"
        "background:rgba(80,220,255,.04);"
        "border:1px solid rgba(80,220,255,.1);"
        "border-radius:20px;padding:5px 14px}"
        ".status-dot{"
        "width:6px;height:6px;border-radius:50%;background:#50DC64;"
        "box-shadow:0 0 8px #50DC64;"
        "animation:blink 2s ease-in-out infinite}"
        "@keyframes blink{"
        "0%,100%{opacity:1}"
        "50%{opacity:.3}}"
        ".status-txt{font-size:11px;color:#50DC64;"
        "font-family:'JetBrains Mono',monospace;letter-spacing:.5px}"
        ".badge{font-size:11px;color:#3A4860;"
        "font-family:'JetBrains Mono',monospace;"
        "border-left:1px solid rgba(80,220,255,.1);padding-left:12px;margin-left:4px}"

        /* ── Nav ────────────────────────────────────────────────── */
        "#nav{"
        "position:relative;z-index:10;"
        "background:#060810;"
        "border-bottom:1px solid rgba(80,220,255,.08);"
        "padding:0 28px;display:flex;gap:0}"
        ".tab{"
        "padding:12px 20px;"
        "color:#3A4860;"
        "border-bottom:2px solid transparent;"
        "font-size:12px;font-weight:600;letter-spacing:.8px;text-transform:uppercase;"
        "display:inline-block;"
        "transition:color .2s,border-color .2s,background .2s;"
        "position:relative}"
        ".tab:hover{color:#8AACCC;background:rgba(80,220,255,.03)}"
        ".tab.on{color:#50DCFF;border-bottom-color:#50DCFF;"
        "background:rgba(80,220,255,.04)}"
        ".tab.on::after{"
        "content:'';"
        "position:absolute;bottom:-1px;left:50%;transform:translateX(-50%);"
        "width:40%;height:2px;"
        "background:linear-gradient(90deg,transparent,rgba(80,220,255,.4),transparent)}"

        /* ── Page body ──────────────────────────────────────────── */
        "#body{position:relative;z-index:1;padding:24px 28px;max-width:1140px;margin:0 auto}"

        /* ── Section labels ─────────────────────────────────────── */
        ".sec{"
        "font-size:9px;font-weight:700;letter-spacing:3px;text-transform:uppercase;"
        "color:rgba(80,220,255,.3);margin:24px 0 10px;"
        "display:flex;align-items:center;gap:10px}"
        ".sec::after{"
        "content:'';flex:1;height:1px;"
        "background:linear-gradient(90deg,rgba(80,220,255,.15),transparent)}"
        ".sec:first-child{margin-top:0}"

        /* ── Cards ──────────────────────────────────────────────── */
        ".card{"
        "background:linear-gradient(180deg,rgba(16,24,48,.8) 0%,rgba(8,14,28,.8) 100%);"
        "border:1px solid rgba(80,220,255,.08);"
        "border-radius:12px;overflow:hidden;margin-bottom:14px;"
        "backdrop-filter:blur(4px);"
        "transition:border-color .2s,box-shadow .2s;"
        "position:relative}"
        ".card::before{"
        "content:'';"
        "position:absolute;top:0;left:0;right:0;height:1px;"
        "background:linear-gradient(90deg,transparent,rgba(80,220,255,.1),transparent)}"
        ".card:hover{"
        "border-color:rgba(80,220,255,.18);"
        "box-shadow:0 0 30px rgba(80,220,255,.04)}"
        ".card-head{"
        "padding:11px 18px;"
        "border-bottom:1px solid rgba(80,220,255,.06);"
        "display:flex;align-items:center;gap:10px}"
        ".card-icon{"
        "width:8px;height:8px;border-radius:50%;flex-shrink:0;"
        "box-shadow:0 0 8px currentColor}"
        ".card-title{"
        "font-size:9px;font-weight:700;letter-spacing:2.5px;"
        "text-transform:uppercase;color:rgba(160,180,210,.4)}"
        ".card-body{padding:4px 0}"

        /* ── Data rows ──────────────────────────────────────────── */
        ".row{"
        "display:grid;grid-template-columns:140px 1fr;"
        "padding:9px 18px;"
        "border-bottom:1px solid rgba(80,220,255,.04);"
        "align-items:center;transition:background .15s}"
        ".row:last-child{border-bottom:none}"
        ".row:hover{background:rgba(80,220,255,.03)}"
        ".row .lbl{"
        "font-size:10px;color:rgba(100,130,160,.6);"
        "font-weight:600;letter-spacing:.8px;text-transform:uppercase}"
        ".row .val{"
        "font-size:13px;color:#C0CCD8;"
        "font-family:'JetBrains Mono',monospace;font-weight:400}"

        /* ── Value colours ──────────────────────────────────────── */
        ".cy{color:#50DCFF;text-shadow:0 0 12px rgba(80,220,255,.3)}"
        ".gn{color:#50DC64;text-shadow:0 0 12px rgba(80,220,100,.3)}"
        ".rd{color:#FF5060;text-shadow:0 0 12px rgba(255,80,96,.3)}"
        ".or{color:#FFA028;text-shadow:0 0 12px rgba(255,160,40,.3)}"
        ".dm{color:#2A3040}"
        ".wh{color:#E8F0FF}"

        /* ── Two-column grid ────────────────────────────────────── */
        ".grid2{display:grid;grid-template-columns:1fr 1fr;gap:14px}"

        /* ── Report pre ─────────────────────────────────────────── */
        "pre.rpt{"
        "background:#030508;"
        "color:#00E87A;"
        "border:1px solid rgba(0,232,122,.12);"
        "border-radius:12px;"
        "padding:24px;overflow-x:auto;"
        "font-size:12px;line-height:1.8;"
        "white-space:pre-wrap;word-break:break-all;"
        "box-shadow:inset 0 0 40px rgba(0,232,122,.03)}"

        /* ── Info box ───────────────────────────────────────────── */
        ".info{"
        "background:rgba(80,220,255,.03);"
        "border:1px solid rgba(80,220,255,.1);"
        "border-left:2px solid #50DCFF;"
        "border-radius:0 10px 10px 0;"
        "padding:12px 18px;margin:12px 0;"
        "font-size:13px;color:#5A7090;line-height:1.6}"
        ".info strong{color:#9AAAC0}"
        ".info.ok{border-left-color:#50DC64;background:rgba(80,220,100,.03);border-color:rgba(80,220,100,.1)}"
        ".info.err{border-left-color:#FF5060;background:rgba(255,80,96,.03);border-color:rgba(255,80,96,.1)}"

        /* ── File cards ─────────────────────────────────────────── */
        ".fcard{"
        "background:linear-gradient(180deg,rgba(12,20,40,.7),rgba(6,10,20,.7));"
        "border:1px solid rgba(80,220,255,.08);"
        "border-radius:10px;"
        "padding:14px 18px;display:flex;align-items:center;gap:14px;"
        "margin-bottom:8px;transition:all .2s}"
        ".fcard:hover{border-color:rgba(80,220,255,.2);background:rgba(80,220,255,.03)}"
        ".fcard .fname{font-size:13px;font-weight:500;color:#C0CCD8;"
        "font-family:'JetBrains Mono',monospace}"
        ".fcard .fdesc{font-size:11px;color:#3A4860;margin-top:3px}"
        ".fcard .fmeta{margin-left:auto;text-align:right;flex-shrink:0}"
        ".fcard .fsize{font-size:11px;color:#3A4860;"
        "font-family:'JetBrains Mono',monospace}"
        ".badge-ok{display:inline-flex;align-items:center;gap:4px;"
        "background:rgba(80,220,100,.08);border:1px solid rgba(80,220,100,.2);"
        "color:#50DC64;border-radius:4px;padding:2px 10px;"
        "font-size:11px;margin-top:4px;letter-spacing:.3px}"
        ".badge-no{display:inline-block;"
        "background:rgba(255,255,255,.02);border:1px solid rgba(255,255,255,.05);"
        "color:#2A3040;border-radius:4px;padding:2px 10px;"
        "font-size:11px;margin-top:4px}"
        ".badge-dl{display:inline-flex;align-items:center;gap:4px;"
        "background:rgba(80,220,255,.06);border:1px solid rgba(80,220,255,.2);"
        "color:#50DCFF;border-radius:4px;padding:2px 10px;"
        "font-size:11px;margin-top:4px;letter-spacing:.3px;"
        "transition:background .15s}"
        ".badge-dl:hover{background:rgba(80,220,255,.12)}"

        /* ── Settings cards ─────────────────────────────────────── */
        ".scard{"
        "background:linear-gradient(180deg,rgba(14,22,44,.8),rgba(8,12,24,.8));"
        "border:1px solid rgba(80,220,255,.08);"
        "border-radius:12px;overflow:hidden;margin-bottom:14px;"
        "transition:border-color .2s}"
        ".scard:hover{border-color:rgba(80,220,255,.15)}"
        ".scard-head{padding:11px 18px;border-bottom:1px solid rgba(80,220,255,.06);"
        "display:flex;align-items:center;gap:10px}"
        ".scard-body{padding:0}"
        ".srow{display:flex;align-items:center;padding:13px 18px;"
        "border-bottom:1px solid rgba(80,220,255,.04);gap:16px;"
        "transition:background .15s}"
        ".srow:last-child{border-bottom:none}"
        ".srow:hover{background:rgba(80,220,255,.03)}"
        ".srow .slbl{font-size:13px;color:#A0B0C8;font-weight:500;flex:1}"
        ".srow .sdesc{font-size:11px;color:#2A3850;margin-top:3px;line-height:1.4}"

        /* ── Toggle ─────────────────────────────────────────────── */
        ".tog{position:relative;width:46px;height:25px;flex-shrink:0}"
        ".tog input{opacity:0;width:0;height:0;position:absolute}"
        ".togslider{"
        "position:absolute;inset:0;"
        "background:rgba(80,220,255,.06);"
        "border:1px solid rgba(80,220,255,.15);"
        "border-radius:25px;cursor:pointer;transition:all .25s}"
        ".togslider:before{"
        "content:'';"
        "position:absolute;width:17px;height:17px;"
        "left:3px;top:3px;"
        "background:#3A4860;"
        "border-radius:50%;"
        "transition:transform .25s,background .25s,box-shadow .25s;"
        "box-shadow:0 1px 4px rgba(0,0,0,.5)}"
        ".tog input:checked+.togslider{"
        "background:rgba(80,220,100,.12);"
        "border-color:rgba(80,220,100,.35)}"
        ".tog input:checked+.togslider:before{"
        "transform:translateX(21px);"
        "background:#50DC64;"
        "box-shadow:0 0 8px rgba(80,220,100,.6)}"

        /* ── Number inputs ──────────────────────────────────────── */
        ".numinp{"
        "background:rgba(80,220,255,.04);"
        "border:1px solid rgba(80,220,255,.15);"
        "border-radius:6px;"
        "color:#50DCFF;"
        "font-family:'JetBrains Mono',monospace;font-size:14px;"
        "padding:5px 8px;width:66px;text-align:center;transition:all .2s}"
        ".numinp:focus{"
        "outline:none;"
        "border-color:rgba(80,220,255,.5);"
        "background:rgba(80,220,255,.08);"
        "box-shadow:0 0 0 3px rgba(80,220,255,.08)}"
        ".dur-pair{display:flex;align-items:center;gap:8px;font-size:12px;color:#2A3850}"

        /* ── Sticky save bar ────────────────────────────────────── */
        "#savebar{"
        "position:sticky;bottom:0;"
        "background:rgba(6,8,15,.92);"
        "backdrop-filter:blur(12px);"
        "-webkit-backdrop-filter:blur(12px);"
        "border-top:1px solid rgba(80,220,255,.1);"
        "padding:14px 28px;display:flex;align-items:center;gap:14px;"
        "z-index:10;margin:0 -28px}"
        ".savebtn{"
        "background:linear-gradient(135deg,rgba(80,220,100,.15),rgba(60,180,80,.1));"
        "border:1px solid rgba(80,220,100,.3);"
        "color:#50DC64;"
        "font-family:'Inter',sans-serif;font-size:13px;font-weight:600;"
        "padding:9px 28px;border-radius:8px;cursor:pointer;transition:all .2s;"
        "letter-spacing:.3px;"
        "box-shadow:0 0 20px rgba(80,220,100,.05)}"
        ".savebtn:hover{"
        "background:linear-gradient(135deg,rgba(80,220,100,.25),rgba(60,180,80,.18));"
        "border-color:rgba(80,220,100,.6);"
        "box-shadow:0 0 24px rgba(80,220,100,.15)}"

        /* ── About ──────────────────────────────────────────────── */
        ".about-hero{text-align:center;padding:40px 0 28px}"
        ".about-logos{"
        "display:flex;justify-content:center;align-items:center;"
        "gap:40px;flex-wrap:wrap;padding:28px 0 40px}"

        /* ── Report pills ───────────────────────────────────────── */
        ".filepicker{display:flex;flex-wrap:wrap;gap:8px;margin-top:16px}"
        ".fpill{"
        "background:rgba(80,220,255,.04);"
        "border:1px solid rgba(80,220,255,.1);"
        "border-radius:6px;padding:5px 14px;"
        "font-size:11px;color:#3A5878;"
        "font-family:'JetBrains Mono',monospace;transition:all .2s}"
        ".fpill:hover{border-color:rgba(80,220,255,.4);color:#50DCFF;"
        "background:rgba(80,220,255,.08)}"

        /* ── Stress banner ──────────────────────────────────────── */
        ".stress-banner{"
        "background:linear-gradient(90deg,rgba(255,160,40,.06),rgba(255,160,40,.02));"
        "border:1px solid rgba(255,160,40,.2);"
        "border-left:2px solid #FFA028;"
        "border-radius:0 10px 10px 0;"
        "padding:12px 18px;margin-bottom:18px;"
        "display:flex;align-items:center;gap:10px;font-size:13px;color:#7A6040}"
        ".stress-pulse{"
        "width:8px;height:8px;border-radius:50%;background:#FFA028;"
        "box-shadow:0 0 10px #FFA028;"
        "animation:blink 1s ease-in-out infinite;flex-shrink:0}"

        /* ── Footer ─────────────────────────────────────────────── */
        "#ftr{"
        "position:relative;z-index:1;"
        "margin-top:40px;padding:16px 28px;"
        "border-top:1px solid rgba(80,220,255,.06);"
        "display:flex;justify-content:space-between;align-items:center;"
        "font-size:11px;color:#1E2840}"

        /* ── Mobile ─────────────────────────────────────────────── */
        "@media(max-width:640px){"
        ".grid2{grid-template-columns:1fr}"
        "#body{padding:14px 16px}"
        "#hdr-inner{padding:10px 0}"
        "#hdr{padding:0 16px}"
        ".badge,.hdr-status .badge{display:none}"
        "#nav{padding:0 16px}"
        ".tab{padding:10px 14px;font-size:11px;letter-spacing:.5px}"
        ".row{grid-template-columns:110px 1fr;padding:8px 14px}"
        ".card-head,.scard-head{padding:10px 14px}"
        ".card-body,.scard-body .srow{padding-left:14px;padding-right:14px}"
        ".fcard{flex-wrap:wrap}"
        ".fcard .fmeta{margin-left:0;margin-top:8px;width:100%;"
        "display:flex;gap:8px;align-items:center;text-align:left}"
        ".srow{flex-wrap:wrap;gap:10px}"
        ".dur-pair{flex-wrap:wrap}"
        "#savebar{margin:0 -16px;padding:12px 16px}"
        ".about-logos{gap:24px}"
        "}"
        "</style>");
}

static void PageStart(const char* title, const char* tab,
    const char* extraHead = NULL)
{
    s_bufLen = 0;
    EmitCSS();
    BA("<title>"); BAE(title); BA(" - XbDiag</title>");
    if (extraHead) BA(extraHead);
    BA("</head><body>");

    // Header
    BA("<div id=hdr><div id=hdr-inner>"
        "<div class=logoblock>"
        "<img src='data:image/png;base64,");
    BA(k_img_xbdiag);
    BA("' alt='XbDiag' style='width:40px;height:40px;border-radius:8px;"
        "box-shadow:0 0 20px rgba(80,220,255,.2)'>"
        "<div class=logoring></div>"
        "</div>"
        "<div>"
        "<div class=brand>XbDiag</div>"
        "<div class=sub>Original Xbox Hardware Diagnostic &mdash; Team Resurgent &bull; Darkone Customs</div>"
        "</div>"
        "<div class=hdr-status>"
        "<span class=status-dot></span>"
        "<span class=status-txt>ONLINE</span>"
        "<span class=badge>:80</span>"
        "</div>"
        "</div></div>");

    // Nav
    BA("<div id=nav>");
    BA("<a class='tab"); if (SEq(tab, "si")) BA(" on"); BA("' href=/sysinfo>System Info</a>");
    BA("<a class='tab"); if (SEq(tab, "rp")) BA(" on"); BA("' href=/report>Report</a>");
    BA("<a class='tab"); if (SEq(tab, "st")) BA(" on"); BA("' href=/settings>Settings</a>");
    BA("<a class='tab"); if (SEq(tab, "ab")) BA(" on"); BA("' href=/about>About</a>");
    BA("</div><div id=body>");
}

static void PageEnd()
{
    BA("</div>"
        "<div id=ftr>"
        "<span>XbDiag &mdash; Team Resurgent / Darkone Customs</span>"
        "<span class=mono style='font-size:10px;color:#1A2438'>"
        "HTTP/1.0 &bull; port 80 &bull; original xbox</span>"
        "</div></body></html>");
}

static void CardStart(const char* title, const char* iconCls)
{
    BA("<div class=card><div class=card-head>"
        "<span class='card-icon "); BA(iconCls); BA("'></span>"
            "<span class=card-title>"); BAE(title); BA("</span>"
                "</div><div class=card-body>");
}

static void CardEnd() { BA("</div></div>\n"); }

static void Row(const char* lbl, const char* val, const char* cls)
{
    BA("<div class=row><div class=lbl>"); BAE(lbl);
    BA("</div><div class='val "); BA(cls); BA("'>");
    BAE(val ? val : "---"); BA("</div></div>\n");
}

// ============================================================================
// Icon colour classes
// ============================================================================

static const char* IC_CPU = "cy";   // maps to .card-icon colour via inline style
static const char* IC_MEM = "gn";
static const char* IC_BRD = "or";
static const char* IC_BIOS = "dm";   // actually gray
static const char* IC_VID = "cy";
static const char* IC_TMP = "rd";
static const char* IC_HDD = "cy";
static const char* IC_NET = "gn";

static void CardStartC(const char* title, const char* color)
{
    BA("<div class=card><div class=card-head>"
        "<span class=card-icon style='background:");
    BA(color); BA("'></span>"
        "<span class=card-title>"); BAE(title); BA("</span>"
            "</div><div class=card-body>");
}

// ============================================================================
// BuildSysInfo
// ============================================================================

static void BuildSysInfo()
{
    SysSnapshot snap;
    bool hasData;
    bool cpuRun, ramRun, gpuRun;

    ZeroMemory(&snap, sizeof(snap));
    hasData = SysInfo_GetSnapshot(snap);

    // Auto-refresh every 30 seconds for live temps
    PageStart("System Info", "si",
        "<meta http-equiv=refresh content=30>");

    // Stress status banner if anything is running
    cpuRun = (s_state == SSTATE_RUNNING);
    ramRun = (s_ramState == SSTATE_RUNNING);
    gpuRun = (s_gpuState == SSTATE_RUNNING);
    if (cpuRun || ramRun || gpuRun)
    {
        BA("<div class=stress-banner>"
            "<span class=stress-pulse></span>"
            "<strong style='color:#FFA028'>Stress Test Active:</strong>&nbsp;");
        if (cpuRun) BA("CPU &nbsp;");
        if (ramRun) BA("RAM &nbsp;");
        if (gpuRun) BA("GPU &nbsp;");
        BA("<span style='margin-left:auto;font-size:11px;color:#4A3820'>"
            "Page refreshes every 30s</span>"
            "</div>\n");
    }

    BA("<div class=grid2>");

    // Left column
    BA("<div>");
    BA("<div class=sec>Hardware</div>");

    CardStartC("CPU", "#50DCFF");
    Row("IC", snap.cpuIC, "cy");
    Row("Speed", snap.cpuSpeed, "wh cy");
    Row("Brand", snap.cpuBrand, "");
    CardEnd();

    CardStartC("Memory", "#50DC64");
    Row("Total", snap.memTotal, "gn wh");
    Row("Config", snap.memConfig, "");
    CardEnd();

    CardStartC("Board", "#FFA028");
    Row("Revision", snap.boardRev, "or wh");
    Row("Serial", snap.serialNum, "");
    Row("Modchip", snap.modchip, "");
    Row("HD Mod", snap.hdMod, "");
    CardEnd();

    CardStartC("BIOS", "#3A4258");
    Row("Version", snap.biosVer, "");
    CardEnd();

    BA("</div>");

    // Right column
    BA("<div>");
    BA("<div class=sec>System</div>");

    CardStartC("Video", "#C87AFF");
    Row("Encoder", snap.encName, "");
    Row("AV Pack", snap.avPack, "");
    Row("Mode", g_videoModeStr, "cy wh");
    Row("GPU Speed", snap.gpuSpeed, "cy");
    CardEnd();

    CardStartC("Thermal", "#DC3C3C");
    Row("CPU Temp", snap.tempCPU, "or wh");
    Row("Ambient Temp", snap.tempAmbient, "");
    CardEnd();

    CardStartC("Storage", "#50DCFF");
    Row("Model", snap.hddModel, "");
    Row("Size", snap.hddSize, "cy wh");
    Row("UDMA", snap.hddUDMA, "");
    CardEnd();

    CardStartC("Network", "#50DC64");
    Row("IP Address", snap.ipAddr,
        (snap.ipAddr && snap.ipAddr[0] && snap.ipAddr[0] != 'N') ? "gn wh" : "dm");
    Row("MAC", snap.macAddr, "");
    CardEnd();

    BA("</div>");
    BA("</div>"); // grid2

    // Stress test live data if running
    if (cpuRun || ramRun || gpuRun)
    {
        BA("<div class=sec>Live Stress Data</div>");
        BA("<div class=grid2>");

        if (cpuRun)
        {
            char t[16];
            CardStartC("CPU Stress", "#FFA028");
            IntToStr((int)StressAutoRun_GetMaxCPU(), t, sizeof(t));
            StrCat2(t, sizeof(t), t, " C");
            Row("Peak Temp", t, "or");
            IntToStr((int)StressAutoRun_GetMinCPU(), t, sizeof(t));
            StrCat2(t, sizeof(t), t, " C");
            Row("Min Temp", t, "");
            IntToStr((int)StressAutoRun_GetMaxFan() * 2, t, sizeof(t));
            StrCat2(t, sizeof(t), t, "%");
            Row("Peak Fan", t, "cy");
            Row("Thermal Abort", StressAutoRun_GetThermalAbort() ? "YES" : "No",
                StressAutoRun_GetThermalAbort() ? "rd" : "gn");
            CardEnd();
        }

        if (ramRun)
        {
            char t[16];
            CardStartC("RAM Stress", "#50DCFF");
            IntToStr((int)RamAutoRun_GetSweeps(), t, sizeof(t));
            Row("Sweeps", t, "cy");
            IntToStr((int)RamAutoRun_GetErrors(), t, sizeof(t));
            Row("Errors", t, RamAutoRun_GetErrors() > 0 ? "rd" : "gn");
            Row("Result", RamAutoRun_GetFailed() > 0 ? "FAIL" : "Pass",
                RamAutoRun_GetFailed() > 0 ? "rd" : "gn");
            CardEnd();
        }

        if (gpuRun)
        {
            char t[16];
            int minfps;
            CardStartC("GPU Stress", "#C87AFF");
            IntToStr((int)GpuAutoRun_GetLoops(), t, sizeof(t));
            Row("Scene Loops", t, "cy");
            IntToStr((int)GpuAutoRun_GetPeakFPS(), t, sizeof(t));
            StrCat2(t, sizeof(t), t, " fps");
            Row("Peak FPS", t, "gn");
            minfps = (GpuAutoRun_GetMinFPS() < 9000.f) ?
                (int)GpuAutoRun_GetMinFPS() : 0;
            IntToStr(minfps, t, sizeof(t));
            StrCat2(t, sizeof(t), t, " fps");
            Row("Min FPS", t, minfps >= 30 ? "gn" : minfps >= 20 ? "or" : "rd");
            CardEnd();
        }

        BA("</div>"); // grid2
    }

    if (!hasData)
        BA("<div class=info><strong>Hardware data not yet available.</strong> "
            "Open System Info from the XbDiag menu to populate this page.</div>\n");
    else
        BA("<div class=info>Page auto-refreshes every 30 seconds. "
            "Re-open System Info on the Xbox to update the snapshot.</div>\n");

    PageEnd();
}

// ============================================================================
// BuildReport -- auto-detects most recently modified report
// ============================================================================

static const char* FindDefaultReport()
{
    int i;
    for (i = 0; i < k_fileCount; ++i)
    {
        if (!k_files[i].isBinary)
        {
            char path[64];
            HANDLE hf;
            StrCopy(path, sizeof(path), "D:\\");
            StrCat2(path, sizeof(path), path, k_files[i].filename);
            hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) { CloseHandle(hf); return k_files[i].filename; }
        }
    }
    return "XbDiag.txt";
}

static void BuildReport(const char* fname)
{
    const char* fn = (fname && fname[0]) ? fname : FindDefaultReport();
    char path[64];
    HANDLE hf;
    StrCopy(path, sizeof(path), "D:\\");
    StrCat2(path, sizeof(path), path, fn);

    PageStart("Report", "rp");

    BA("<div class=sec>Report Viewer</div>");
    BA("<div class=card style='margin-bottom:14px'>"
        "<div class=card-head>"
        "<span class=card-icon style='background:#50DCFF'></span>"
        "<span class=card-title>"); BAE(fn); BA("</span>"
            "</div></div>");

    hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hf == INVALID_HANDLE_VALUE)
    {
        HANDLE hs;
        bool hasSettings;
        hs = CreateFileA("D:\\XbDiag.set", GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        hasSettings = (hs != INVALID_HANDLE_VALUE);
        if (hasSettings) CloseHandle(hs);

        if (hasSettings)
            BA("<div class=info><strong>No report found yet.</strong> "
                "Automation settings are configured. Run the automated sequence "
                "from the XbDiag main menu to generate a report.</div>\n");
        else
            BA("<div class=info><strong>No report found.</strong> "
                "Open <strong>Settings</strong> to configure automation, then run "
                "the automated sequence from the XbDiag main menu.</div>\n");
    }
    else
    {
        DWORD nr = 0;
        ReadFile(hf, s_fileReadBuf, k_fileReadSize - 1, &nr, NULL);
        CloseHandle(hf);
        s_fileReadBuf[nr] = '\0';
        if (nr == 0)
            BA("<div class=info>File is empty.</div>\n");
        else
        {
            BA("<pre class=rpt>"); BAE(s_fileReadBuf); BA("</pre>\n");
            if (nr >= (DWORD)(k_fileReadSize - 1))
                BA("<div class=info>Truncated at 32 KB for display.</div>\n");
        }
    }

    // File picker pills (text files only)
    BA("<div class=sec style='margin-top:20px'>Other Reports</div>");
    BA("<div class=filepicker>");
    {
        int i;
        for (i = 0; i < k_fileCount; ++i)
        {
            if (!k_files[i].isBinary)
            {
                BA("<a class=fpill href='/report?f=");
                BAE(k_files[i].filename);
                BA("'>"); BAE(k_files[i].filename); BA("</a>");
            }
        }
    }
    BA("</div>\n");

    PageEnd();
}

// ============================================================================
// Settings helpers
// ============================================================================

static void SCardStart(const char* title, const char* color)
{
    BA("<div class=scard><div class=scard-head>"
        "<span class=card-icon style='background:");
    BA(color); BA("'></span>"
        "<span class=card-title>"); BAE(title); BA("</span>"
            "</div><div class=scard-body>");
}

static void SCardEnd() { BA("</div></div>\n"); }

// Toggle row
static void SToggle(const char* name, const char* label,
    const char* desc, bool checked)
{
    BA("<div class=srow>"
        "<div><div class=slbl>"); BAE(label); BA("</div>"
            "<div class=sdesc>"); BAE(desc); BA("</div></div>"
                "<label class=tog>"
                "<input type=checkbox name='"); BAE(name); BA("' value=1");
    if (checked) BA(" checked");
    BA("><span class=togslider></span></label>"
        "</div>\n");
}

// Duration row (hours + mins)
static void SDuration(const char* nameH, const char* nameM,
    const char* label, int hours, int mins)
{
    BA("<div class=srow>"
        "<div class=slbl>"); BAE(label); BA("</div>"
            "<div class=dur-pair>"
            "<input class=numinp type=number name='"); BAE(nameH);
    BA("' value='"); BAI(hours);
    BA("' min=0 max=99> <span>hrs</span>"
        "<input class=numinp type=number name='"); BAE(nameM);
    BA("' value='"); BAI(mins);
    BA("' min=0 max=59> <span>min</span>"
        "</div></div>\n");
}

// Number row
static void SNumber(const char* name, const char* label,
    const char* desc, int val, int lo, int hi)
{
    BA("<div class=srow>"
        "<div><div class=slbl>"); BAE(label); BA("</div>"
            "<div class=sdesc>"); BAE(desc); BA("</div></div>"
                "<input class=numinp type=number name='"); BAE(name);
    BA("' value='"); BAI(val);
    BA("' min='"); BAI(lo);
    BA("' max='"); BAI(hi); BA("'>"
        "</div>\n");
}

// ============================================================================
// BuildSettings -- full XbSet editor
// ============================================================================

static void BuildSettings()
{
    PageStart("Settings", "st");

    // Save result feedback
    if (s_saveAttempted)
    {
        if (s_saveOK)
            BA("<div class='info ok'><strong>Settings saved</strong> to D:\\XbDiag.set</div>\n");
        else
            BA("<div class='info err'><strong>Save failed</strong> — could not write D:\\XbDiag.set</div>\n");
        s_saveAttempted = false;
    }

    BA("<form method='GET' action='/save'>\n");

    // Diagnostic modules
    BA("<div class=sec>Diagnostic Modules</div>");
    SCardStart("Modules", "#50DCFF");
    SToggle("SYSINFO", "System Info", "Full hardware snapshot", g_autoSettings.runSysInfo);
    SToggle("VIDEOINFO", "Video Info", "Encoder, AV pack, resolution", g_autoSettings.runVideoInfo);
    SToggle("SMBUS", "SMBus Scan", "Scan all SMBus addresses", g_autoSettings.runSmBus);
    SToggle("HDDINFO", "HDD Info", "ATA identify and sequential benchmark", g_autoSettings.runHddInfo);
    SToggle("RAMTEST", "RAM Test", "One full quick sweep across all banks", g_autoSettings.runRamTest);
    SToggle("TEMPMON", "Temp Monitor", "5 temperature samples at 500ms intervals", g_autoSettings.runTempMon);
    SToggle("EEPROM", "EEPROM", "Read and decode EEPROM contents", g_autoSettings.runEeprom);
    SToggle("CTRLTEST", "Controller Test", "60s wait for input, skip if none", g_autoSettings.runCtrlTest);
    SCardEnd();

    // Stress tests
    BA("<div class=sec>Stress Tests</div>");
    BA("<div class=grid2>");

    SCardStart("CPU Stress", "#FFA028");
    SToggle("CPUSTRESS", "Enable", "Run CPU FPU burn", g_autoSettings.runCpuStress);
    SDuration("CPU_HRS", "CPU_MIN", "Duration", g_autoSettings.cpuStressHours, g_autoSettings.cpuStressMins);
    SCardEnd();

    SCardStart("RAM Stress", "#50DC64");
    SToggle("RAMSTRESS", "Enable", "Repeated RAM sweeps", g_autoSettings.runRamStress);
    SDuration("RAM_HRS", "RAM_MIN", "Duration", g_autoSettings.ramStressHours, g_autoSettings.ramStressMins);
    SCardEnd();

    SCardStart("GPU Stress", "#C87AFF");
    SToggle("GPUSTRESS", "Enable", "Crystalline Grotto NV2A scene loop", g_autoSettings.runGpuStress);
    SDuration("GPU_HRS", "GPU_MIN", "Duration", g_autoSettings.gpuStressHours, g_autoSettings.gpuStressMins);
    SCardEnd();

    BA("</div>"); // grid2

    // Run options
    BA("<div class=sec>Run Options</div>");
    SCardStart("Options", "#3A4258");
    SNumber("LOOPS", "Stress Loops", "Repeat the full stress sequence N times", g_autoSettings.stressLoops, 1, 99);
    SToggle("ALT_STRESS", "Interleave Stress", "Alternate CPU/RAM/GPU per loop instead of all-CPU then all-RAM then all-GPU", g_autoSettings.altStressMode);
    SToggle("SHUTDOWN", "Shutdown After", "Power off Xbox automatically when run completes", g_autoSettings.shutdownAfter);
    SCardEnd();

    // Sticky save bar
    BA("<div id=savebar>"
        "<button class=savebtn type=submit>&#10003; Save Settings</button>"
        "<span style='font-size:12px;color:#3A4258'>Saves to D:\\XbDiag.set</span>"
        "</div>\n");

    BA("</form>\n");

    // Export files section
    BA("<div class=sec style='margin-top:24px'>Export Files</div>");
    BA("<div class=info>Files saved to <code>D:\\</code> by XbDiag modules. "
        "Enable FTP in File Explorer to transfer files to your PC. "
        "Binary files can be downloaded directly from this page.</div>\n");

    {
        int i;
        for (i = 0; i < k_fileCount; ++i)
        {
            const char* fn = k_files[i].filename;
            char path[64];
            HANDLE hf;
            bool exists;
            DWORD fsize = 0;

            StrCopy(path, sizeof(path), "D:\\");
            StrCat2(path, sizeof(path), path, fn);
            hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            exists = (hf != INVALID_HANDLE_VALUE);
            if (exists) { fsize = GetFileSize(hf, NULL); CloseHandle(hf); }

            BA("<div class=fcard>");
            BA("<span class=card-icon style='background:");
            BA(exists ? "#50DCFF" : "#2A3040");
            BA(";width:8px;height:8px;flex-shrink:0'></span>");

            BA("<div style='flex:1;min-width:0'>");
            if (exists && !k_files[i].isBinary)
            {
                BA("<a class=fname href='/report?f="); BAE(fn); BA("'>"); BAE(fn); BA("</a>");
            }
            else { BA("<div class=fname>"); BAE(fn); BA("</div>"); }
            BA("<div class=fdesc>"); BAE(k_files[i].desc); BA("</div>");
            BA("</div>");

            BA("<div class=fmeta>");
            if (exists) { BA("<div class=fsize>"); BAI((int)(fsize / 1024 + 1)); BA(" KB</div>"); }
            if (exists && k_files[i].isBinary)
            {
                BA("<a class=badge-dl href='/download?f="); BAE(fn); BA("'>&#8595; Download</a>");
            }
            else if (exists)
                BA("<span class=badge-ok>&#10003; Present</span>");
            else
                BA("<span class=badge-no>Not found</span>");
            BA("</div>");
            BA("</div>\n");
        }
    } // file loop scope

    PageEnd();
}

// ============================================================================
// SaveSettings from POST body -- exact XbDiag.set format
// ============================================================================

static bool SaveSettingsFromPost(const char* body)
{
    // Guard -- empty body means POST read failed
    if (!body || !body[0]) return false;

    g_autoSettings.runSysInfo = ParamBool(body, "SYSINFO");
    g_autoSettings.runVideoInfo = ParamBool(body, "VIDEOINFO");
    g_autoSettings.runSmBus = ParamBool(body, "SMBUS");
    g_autoSettings.runHddInfo = ParamBool(body, "HDDINFO");
    g_autoSettings.runRamTest = ParamBool(body, "RAMTEST");
    g_autoSettings.runTempMon = ParamBool(body, "TEMPMON");
    g_autoSettings.runEeprom = ParamBool(body, "EEPROM");
    g_autoSettings.runCtrlTest = ParamBool(body, "CTRLTEST");
    g_autoSettings.runCpuStress = ParamBool(body, "CPUSTRESS");
    g_autoSettings.runRamStress = ParamBool(body, "RAMSTRESS");
    g_autoSettings.runGpuStress = ParamBool(body, "GPUSTRESS");
    g_autoSettings.altStressMode = ParamBool(body, "ALT_STRESS");
    g_autoSettings.shutdownAfter = ParamBool(body, "SHUTDOWN");

    g_autoSettings.cpuStressHours = ParamInt(body, "CPU_HRS", 0, 99, 0);
    g_autoSettings.cpuStressMins = ParamInt(body, "CPU_MIN", 0, 59, 30);
    g_autoSettings.ramStressHours = ParamInt(body, "RAM_HRS", 0, 99, 0);
    g_autoSettings.ramStressMins = ParamInt(body, "RAM_MIN", 0, 59, 30);
    g_autoSettings.gpuStressHours = ParamInt(body, "GPU_HRS", 0, 99, 0);
    g_autoSettings.gpuStressMins = ParamInt(body, "GPU_MIN", 0, 59, 30);
    g_autoSettings.stressLoops = ParamInt(body, "LOOPS", 1, 99, 1);

    return XbSet_SaveSettings();
}

// ============================================================================
// Embedded images (base64 PNG)
// ============================================================================

static const char* s_img_xbdiag =
"iVBORw0KGgoAAAANSUhEUgAAAHgAAAB4CAYAAAA5ZDbSAAABCGlDQ1BJQ0MgUHJvZmlsZQAAeJxj"
"YGA8wQAELAYMDLl5JUVB7k4KEZFRCuwPGBiBEAwSk4sLGHADoKpv1yBqL+viUYcLcKakFicD6Q9A"
"rFIEtBxopAiQLZIOYWuA2EkQtg2IXV5SUAJkB4DYRSFBzkB2CpCtkY7ETkJiJxcUgdT3ANk2uTml"
"yQh3M/Ck5oUGA2kOIJZhKGYIYnBncAL5H6IkfxEDg8VXBgbmCQixpJkMDNtbGRgkbiHEVBYwMPC3"
"MDBsO48QQ4RJQWJRIliIBYiZ0tIYGD4tZ2DgjWRgEL7AwMAVDQsIHG5TALvNnSEfCNMZchhSgSKe"
"DHkMyQx6QJYRgwGDIYMZAKbWPz9HbOBQAABaLUlEQVR42u29d5hdV3nv/1lr7b1Pnz4jzaj3Zku2"
"JfeGbWxjsI0NpppmOuQSQiAkubkQUigJCQkkQCCUBGMgQLDBBmPcC+5FsmT13kbT58xpu621fn+s"
"M6ORMfciLrrPvfl5P895JI3O7LPPetf7rrd83+8rhBAWwFrLi9d/nUsIAYD3onD/a16TMhXAi9L9"
"L3zJF5fgRQG/eL0o4BevFwX84vWigF+8XhTwi9eLAn7xelHA/z+6vBN36+fnUAQgplJoAuF+xOTb"
"mu8Xdtq/j70s9ld/KgArjv1IcfSzj30K+2ueUWBt89emfmaP3sv+L74mtvn7Ap6XFZy6xdSPjXtv"
"86YnOst0AjJZAiEkYBEohPQQQiFQOJnK5p/i+as07fePCmdyQ7h/man3CvF80U8Xl9s8QgjE5FLa"
"aQv6vH0nAGvN0Zvao28RHBUgvyo6ph7GWoQQWGuPfoZwKUNj7dSHWmtdGtFqrE0xVoM1WMz/GwIW"
"0kdKHyU9BP7RJZr6FHV0iZ63apPCmFSkF8qRv4CSPE/Y03X1qGZLd8NjFNxME5MFpJi+A5ofJKbp"
"/5SWTlqTpvY37yCwL/gw7vfstA08fdkNxiTuZROsTf/vFLAQEk/lUCpoClFgLFhzrAY47T5WeJM7"
"/5i/v8CTTWrzCwteghUYcYxuYXEao5yIQRiMMYBCCIFpHglO290DCyEmjbQTvG1uPnFUtYUAjUBY"
"M6XlL7SUR595csHV0aMI0dwPk0eCRtsIY0JnUf7vELBASQ/fLyCEjzUCYw3Wimnaa4/rIew0TZ5c"
"7KZxdwvVvJ8RTmOOfpLECtHcVeKo8bZHrYLAki9kSWNNHKdThlE0f09gQacIobBKIezRzWmaZl9a"
"MNa6z5p8YuE+SzalaZjcNEfV+Bi/4xgr5gTtntFiTIQ2dazV/9vikf+7wvVUFt8vIQjQGrQBa+Wx"
"whUv6DM1DRQYhNvZVroXEoQCoTCTPxMKPSlA4d4jhMKmBqvBIt35ZvQxJlxY6bRbuHsYbehaNJNs"
"Wx5t0mmOFSAk1hiC2asQhTb3ZZj8LtIJyE6aXHnUT5DNL2gl1rg/hZBYKzBW0Nw2GPHr1tFOnd3u"
"3hmUcgrzaxfuRGuwQCBlBt8rYZEYY19gZx49D6V15tAeY0AFU+7W80y30ygBKKwMMChAI02CQDef"
"3NCybg7x/gkaA2UnCyuajrjbWFIodKoBifQUWIPWidNEKRBSYsykY2axJsZvm4MJy5iw3txoBmEE"
"KPd8EjFlQq1OsRiEChDGTPOamxoumGaKp9moqePaTDu+3PHk/gRBijZVjE1+a02Wv+2+kDKLp4pY"
"qzDmaCgx5SVOqahF5zTJSR5kUic4wZQWgkRmMgjlg5HO80aBpilcic34tL71lWQvPc9pqlQgPYyB"
"wsJusvM7mpbCazpAoum5e5jUUDi5j8yCTmdWpUD5AbL5/2mSYK1wn2MFQubQjTFMHCOk39xkApvT"
"zgALr7nZJMZqvHnd5FcvcsfClEcukPkM0vPchqPpXU+Gf8ZghZzSbF7Qt7AIfJQsIoX/f9ZES+Gh"
"ZB6L1zxv9ZQz83wt18TMv24tv/elv6JwZh8oD2UUAomQEms0uXltLHrHOvDN5CmGLOWxfgYrPIRJ"
"6bzkbLInLcFq7RYehfADDn73cUYf24/0c2AVKt+O8DyMkFilIKPoPWsVnavngk4Q0gMUqlDE2JS+"
"85dRmNOBTTUoDyskqqUdIT0MFpNJEX0J2fNbsJ0JeBakRAR5rDG0rl1CYW4PxiSIfAERZAhmtdL5"
"1nMRoumAWeN+p7nxgtZehFBNMbqjSUzz/SfX0QoLwkfJ7NSRcLyXAj5xfN6yQMkcUmZ/rfNkp52v"
"ws+T7WmlvbuNHT9/jmQoRPux01brzrZktEb9wAhW+NjEYo1FtRQQSiEyChGnjN7+IPFTm/DbA2xk"
"EEGAzOWwqUYo57ULqRAKTJKCUAjhIfNFhp/YTmXHEJ4fuHNSSUycYHREy/wZVA5PuM8VEiEFplYD"
"qcCziL6Q2VfN45p3volt49sxwym25kO2BSUV1Wd3Utt+CCkDZGsbKB89UaGx+SAmTEH6iEwBG9VA"
"Ok/fa51BGlYRRjedRIEU8ldOzKmkkPAA/VuFUMctYNl0AKwVxzzIVDhwTAwqEVJR3T3A5vuexB6O"
"8ed5vPyvXsXYeJna/glUJoNFkJQjTGxBCaxUmHoCRiOsIK3XEcbgKVAtPnoicVYjiRBNx0l4ChPH"
"mLiG39mGsNKZ6zR2G0U6rxihQGuKyzsI+joZfmI/RkiEpxBSIgPllkV5ULR0ndfH1W99HR9+ydvY"
"UN3D0N4jxIdiRBQ1A2eF9D1nuit1bBRitcWEGqE8hLWULn0FmTULiTbvQAhJWp9AYpoelTutnI8t"
"j43ipyV5nNOWNNXmBArYeXeZY8yJC2XEsbk5BBgLqUYKHz8KMFj8Ho81F61iz/pd1PZVkChs0yOe"
"PJ9cBsxHWDDZHBde/1qO7O8nrTcwsZnysq0A4UuU72OiiBWvXcX8K5fQv3EQGmAVWCmd5oimtysl"
"JgnpeckKMm0lKvuGkXkPlET4HkgfkxqsJxCpojpcY1e8l43xXu7715uJ1ycQNr3y2C22MMatg/Sa"
"3jYI2dxgRpMO9SPzGZL9/S6zZzRCJ9AMw6Tv0/bGa4j2H8I0Gu53m+srZdNbF85BxCYnzosWQuGr"
"NhB+M8V29DYWiZAe1lMQR1hr8NvasFaQliecBmEwUkKxAUkWT2ScxirPxf2qaQkmzZUEm8+z4uwz"
"2PHwoyQTVax2m9h6QGLxS1k8BPVDQ5zxlYsonZ7j3lfeixjyML5EKokxzp2RGYVRBr/oEZUTSAUq"
"G6BylrTaTIZqwBiw2j2HNHizoLS8RPmBQURcmtow01Nr1qQIK7BGNy2PbMaxKWiNaURIP8CkGiEk"
"fk8LenAUYS0IierpQpcnsFGIwDSjjWOtIyYm0eXjSmseh4AFUgX4shVjxTGZFiskVifk1qwFIWis"
"fwqpArxsgdQaTBg5AfsCG3hYY0Eat5DKgu85WxL44EtQzTgYnDcdhxBkIAWRBqggZfmrlrPlO89h"
"x1KswpnoUgJZUEMSpHJesZBYCargYaMU2iXE4AUtxBM1hBQIJbG1BC0SRMZHGOneay1oEMogSj5m"
"VLg4N+M3iyLNhIqQIIyLm7V2m8NM/t24/xMGlEImKUQJqphBJBqMxSaRs3aeciG/MceY4ikBW0Oi"
"x47rLP6NBSyERKk8ShYw5liPWTcNk8wWMWmCjRO8YisyWyAaOoDws1hPYdMUmVUsOHU+fimPl82j"
"fB98AQEIT4IQeEEGEfguVywU0leY1KI0bHhyE/GeMXKFgLQSoeupWxQBQluMFohAgKfcGWlSVD6L"
"as0TD5cRRR8baoQNkMUseqKOtSm2u4WZ553C6LbdpDsPQ2QxoXHncjaLjgVyyVJOWnsSgQ9Kp3hp"
"jA6rpHGETTVpFJOGkXPy0hhSjdEp1liiRoPD2/aSJCmeNaB8VGsrevAQSEWapoDFm/RilJyKncXk"
"kWdBmzG0iU6MBvteC0JkfiWpYZoOlUv1uVjRKumOeKNBSay2dM5p5Z2ffwPdc3KQS9FWkM0Um+do"
"gPYswvPBy2Clh1aQSNzf0bSLAsUBnw9/8HMMPLiPIJ9BNxoY7ZKbQgmnuUmKUB7aaIQn8HIZlwJU"
"7lkzJZ9kXKOyPia1pPUqC9/9Gp790z/jvz34IP/2e/+dTNggiUGILDpK6b78Yj7+hT+i1hikGpbJ"
"2RSVNkhNDatjPJ2QpnViHVJO6mAMaVinESeIJKWOx5GxmTz5if+gfvAgCndkYTTapHTOXQK5PLUl"
"fZjdu0g3bUD62akq1WRWTtsKqa7+n68Hu7NTucSAUlgjnKmTnqukeBFBHgpzS/z1F77NyX1rWL50"
"BoPsxysFkBeojMJmA8gFiEwAfgbhZ7DKI6Ny7Ih3MDMn+dK//jHvefsnGX78MF42g0hThLXoKAEl"
"8LMeacPQd+pMokrI+KEJtA+qpQ2lE7K9HYTjQ+iRCpk5nRgNo7sO87H7HuCRex9BJGBUgFf0iYdC"
"Oi6/jI/+y0f5zh13EFc95s2eRaVqMFWBl+Qxie80N81gdYq2mljHiDAikZoRDfu//WOWfu3D5JZ0"
"UN23x3ntuln0QOOpAJHN4y9eSDw2hpmKoacVYXh+mfV3bKJ9rw1rJ2MypvK0k0G4mcwj+wXIzkH0"
"dGHjMnbwECKfoscOsnjdPF5+0/Xcd/cmvnnth1ne3UKDhIAcCh+PAI8MEh8IjsnFGmt5/e6vUClU"
"eEtwFu9/26cY3ziC70l0XKNjaZF6XRAdTrBC4BddulMXJGtuOJttt2+h0R+iqyGFeSW6V8xk7x07"
"UEEW4ymsSSHRKKMgEuhaSOniK/jo1z7Cd2++md64wM/e8koCYCMwDIwCR4AhYLD590FgHJhoLm7l"
"41+k85Qu0rt/xv4v/RDZOQ9baSBFgpUamfikaR3wkEEemzZQ1rgy1jH1bYmxDdJ04jc2vL+lgE3z"
"DG4m8rXAYjFSIYVCts9GLD2Jk153CaP9IXu+91Pk8Ha84gRx/x5WX3wmc//xLB783mN88+1/RrJg"
"mKiaklUZhPBc6RHfvaSPwmOpXMlcrw8tLNdt+zK01HmtWse73/zX1LbXEUHIB774Rh64bz3PfP1J"
"/M5OtJAIz6I6siy7cjl77t5FfVxjw5DOU3pZuHYRT37zl3hBQFxLkMo6s10V6AnIn3UuH/q3P+Pm"
"n95O16jlp+94JUWTcGti+G6kCY0lRhAZaBhLVUDFQl0bQqWoSo/0Tz5D12LDzMowm//6i9A6DyFK"
"2HAIsSbFnx8Q/jwlQwu6FoFNnUh9BWl8bLVJSKwNSdLy717AUih8v62Ze55mNoykZWEn0URIY6iK"
"kD7+itWc9Htv4gfvvo79Vc0bPnMbg9+6ET30FH6rJh4aZO3VL0H98VK2fXc9f/yH11LuOURc1yjP"
"QwqLMM3wQbpkf0l08sbs25nr9xER8ZqNXyLboblcruYDb/ok4cEUv0+SJpquzk4G9w+DUvhtWXQj"
"xdRryHwWUSwhfUUyUIOkgWppQZfL9K2bB6nmyLMjmDJkTz6L37vx49z56GP4O0f53vtfxWKruTlN"
"ub4MjQRIdbMcNokeMM08gEQVs8iPfZK2WREz/YTN/+Oz2PaZWNOBrMbYhYO86Z9fycVnruMPPvxZ"
"Jr5jkIlwIZIF6fuYNHFh1NHSGBCR6onfuF78Gyc47VS1Y1rO1LifRWWNDnpcckJ4iNoY4wf28OiR"
"EbaVQ2r7Q2wDpAxIxz2Cnpk89ZN7sJ/bzfyXruSfPnELXcOLqSooNzSVSFAOBeOhZaShGW8o9oXD"
"fHbsK2xo7MPH48ur3srQ7hq/SDfymX/7EEGXJT6colKfeq3GsnWrWXLJGtI0QShJMHcmwfxOlLVY"
"EeCVSniFEu3dXZzzppdTHx4nPFDHjFmyS0/hhq/8CT99+hnYMsKn3nMteQw3RylvGIKoYsjUUlTN"
"4FU1qqpRlQRZTZARZPws8s/+mpbsEHPzgm0f/xy09yLSIoSJCxEjj7TuI20OUQ2YAoP4Cm9mGyZN"
"/+fC+N2fwYpsph2dqqMYI+u8VozFGh/RksNGGqEEalYLbZdfSa2xjOqP70NUn0b4VUy1DjZCtfvo"
"sUOc/sZXMHBxC/aBg1zx4Yt53N9BJlRIJFpJEA7bJaVHTUaU/E4+N+v3mZHLsysp86d3fpWeVa2s"
"TZfzdzd8icZEjCFm0fy5dLz8JJ7+0cPYOISsK0AgJYoAXUmhETL7vBV0trbw7G2PYQct2dnLeOdN"
"f8ldI0OouzfxwQ++mjmBYmgi5v27G9RCjddIXVhjLdIIV6gQAuv7+K0FzD/8LYW2QRYtnMmmj/0N"
"caYdQh+EQvoBQhio1VGrwFvkUb8txFMF0riMkgHC97FpPFVlks2Y2J3FMUla/o01+DdOVQoh8byc"
"Kw1OIQKb9VwlIci65EAaYYzBNDTRjsMkG3egzFakX0U3EvouXstJl5/HgWf34eULHHziSRZ3L2X8"
"lBIHvrWR085bxzYzQBoJQiSRFtQTQy01pDpgJK3xi7GNLM8sQhUkK+Ys465fPMTQ/Igrr7ySZ36+"
"ERMIRkfHqeUs8cAEvcvn0whj5q1bzZkXX8qubduYd/ISaoMV0qxkNK6T7hohN3s1b//ax7ljbAxz"
"5xbe/PtXE2c9xsYafGT9BNVaSlBP0PUY0UiQDYOtpaAdlsMv5OEfPkmucJCFy2ay+eN/T+y3IFpm"
"cskfvInR0WGi/jFEPkB2tWH31NF7DJ7KopOYua9+DWmlQTw2hFBqKg0splCbgNQYE/3GaiyP2+ee"
"DkIVU7gaSCOEruN19CDzbZhaFTMxjq1th7Tq4jlTo1iCmQvzLnGu8sjOXp76+o/oeU4yvLKDJ//i"
"HlaJJQyIhFqoKUeacmyZiAwTNUNUCdg7NsQHNn6NPWNjDGUirnzFNRz4xR5+kdvEm77yPrLZPCKf"
"R+MhCgWinMH4EJRaKXZ1YSJLlBEYa9FCkRxJUe0n89ov/TF3TIxjf/IUV77zcnbnFGPDIX/x4AjV"
"oSr+aA09XkfUQqiG2Ik66BQrJX4+g/3HT5MtHmLhmvls/cQXiLKdqNxMrIAFS9rJ+Q2wKdJq9GAZ"
"LTyQeeIwcUoR6SZcywERaAIAjLXN9L6dBt7jBGiwmtTgKQglaOvqrMrH2phg3hL61q6jxU8pDw0h"
"vGZGsi2HqYbUJ45gCkMMPV1BZLMgFaqYof/Bx1h60joOlBqkt+1h/qWnsicaxkaCKDFoLUgSjYk1"
"IlXU4pBHBrdzUmkBe0s15sxfys4fPcL+U0MuvOAKdt2+ibhdIY5USXIGGprBsMqW8TJy9yBpVxZ9"
"uEKa9/BLC3n1n/4+945USG9+nEveezX7OlooDlf51i8OUx6v4Ichpha6IyhKEFEKEoySBK15+Mrf"
"kFMHWHTGcrZ9/HNE+RJeaSa2brG1GNU5zKEnNrtztyXA1FKkctm9maeuINvVwZEH78FUG6hCDtMI"
"EcYipJzCUDtPWqN1fII0eCr53ayW6JTc4lm0vOwsVEuACkrUt/wSVX6WFdesxto6pAnKF5iRKlZ7"
"ZDoKzFk1u+mBSoc78lpQ7d1s+tJ3mOvNZk+Xz5G/vI9FmdnUbIpOBGEjQSeaJNEkkUbEkuHyCF99"
"8sdkxj12FcY56apLaNy4iftan+Kif34POVUkSSMIMlht8C3kABunKN9VsYK0g0uvuJSH7t6A/fHj"
"nPm2K3kqW8IbKHPLbXspHxnHH69ghsswUYVyFVsNscJipMUvZuArnyMw+5i/bhlb/+ofaGQLyNwM"
"bFW7c1ML+k6ahyplMKl2n1/0sDrB+BGzzp1DW1+AUhIZeMiebgovWUr+lF6ESaZBce1xQ3eOQ8B2"
"Cv46iWwUwiPuHyUeGkdPhJg4xgtK7Lr/fh7/8a284e9uwA8swrdT0BStDdWJEKREK1zhvZJibRHZ"
"XmLLF/6d+S1z2Seh9qnH6G2bRUOmWCNIGpokMsSRJg5jZCwZGB/i5kd/QmEUNuUPsPzVL0N9awuP"
"D/+U0y+7gFxHJ6kHqABdN+g4RRrQaYhfaGPtmrN56Bt3Ej65kbVvfDnrZYnS6BgP3ryN0SNlvKiB"
"rtSxtQg7XoPxBmiLSQ1BNof8ty/g1XbRt2YFOz7zORpWIbM9mEqMTlK076piw5UxtDYgPReNNDR+"
"Fl75hTey/b672XnHHXiFFowxxLt3ko5OEO0faxrZ3x5ZKY/nAJ4ek1lrsVJiwpj6U885hzrrY7RF"
"eS2MbdzH+KE9zFg2k2SsjvV9wJIKQd0KMA0uuv5S5py6FJHNIFraEO0LoG8GOz9/I7MWLGOfSUn+"
"5hG6Cu0kVmOMRMcJNk2xqUHHCV4iGRkb5PEnH6ajWmBTdh/zX3MR3Lidxz757yw68xIynR1Yo5Ey"
"i8zlUB0KaQssXnsmz3z7TgIUp//hu3iuVqA4Ms6mH21kbP8AXtjATNSwjRhqEaqROm9cCvxsHvut"
"zxNM7GbOKYvY+4V/JCx14veuRGVakEGBWavncOF150AS0rAR2iqQPmko0JFmxrJZ2B0DTDy7lyDo"
"JKnWmwiQFhrP7seMhr/SOiOEOFFnsMDzshgjj2kk0VqjvBxC+QipMEmCkDmshWJfjnLV47SXXsNY"
"/yHiSg3ZHZBd3ML4k2MMH9jPxPAwxqYIYbA6RZTy2GxE5a6n6Lvycvp3HSC/eQTOnEVSb4Bx6EoS"
"AbHFaoMwisbgIKZi6Zi3mu2FUeZdeSnlx3YxfMdGOhacTG14hLRYQgdF9N4yM0uL6P/50+SXzOGU"
"v/1DdiR5eoKI/T9+jJFtgyhfQpRAFCNTA0mKLWQw+QxeoYD8z6/iVfYy+8xV7P+3LxNLD9XWBbUY"
"k0QIG9EYH2No/0HSWBOcmmNi0yhmQlLsaWPdyy9koP8Qnlfl8JZxpGoHK5CZrHNYhUTb1GHXJkHy"
"8vjP4N9awBiL7G2j762XUN0xgK00sNqdFyLjYWND64p2RncPM2oOMH5gCCKN7vAQS1upPzVCDJjU"
"YaiMibBp4s71lYvRNqRy9xN0XHMZo9sP4G0Zwp7ai63HDoIbakRiMVGESgw9a87Bu/gljGYV9qBm"
"ZEM/DI/RKENl4SrMla+gcNZZ9C5eSXb1qRwejkh2D9G2cB7lthxhG4x1t+MtnEUmqhLu6kdXGg6t"
"ESeQ9aCUxytkELf8G7K6h551yzh045cJ/Sz+ouVQjUlHQ4fvSizaCuI4wpoUfVJAY+MwjFtiGVKL"
"DhEOReTnZxh6ruyqaMZi0hjSFIp5Oi88nXhkHMIQ5CT0Nj0uAR9XLjqTaUOnXhNgblC97fRedwmH"
"broXE9aRGc89oKew5ZCOl3bTTSf5he3UdsfohsXr9MjOKaJ3JgyMDTLGKF0re7HWEHh5Bp7sJykn"
"+J0Zkl078GNL6d2vZvS+J1CL8uhrF2ErIcJ42HpCsXsWxTPOp2oS6j+6HfvIs4j+EFsziFVrmX3D"
"e7ngsgu51FesAXqBGvBz4GuDVZ76zC3wjR+hujKYrg7UxedRvPJ81GiV+s/XE/WPQDaP7SihZnQh"
"br8RUd/HjLNPZvCmrxFnfVTHHHTDI+jwKa0skdgErKG6u0y37KKnqxMxOybeVUHXA2TR4ncIaoca"
"hLlx+n8xgMjmQKdIBdZqTL7EoutfzaHv30YycAihnCYLmRDHE78xt9lvLWB3EEtSk6Jai2iRQDVF"
"thQdhHZ0AmYa1n3oStrbioQlgy4ovCBAGCi1FZkpuvjW5/+FjlKBUq5EUebpung1j372Hipbx/A6"
"fPTAAXwEhde+jPKDTyFPasNctRRbriC9ErlLriP86e3or/0MmcuTOedUogd3oNZdyEkfeB/LkgTz"
"4KMcGaxyUCpUJsPyrMfFvV2svfw8ftzdwT/+zZ2If/4WnWfOZWz9DuLUIl/5ClrPO4WJmx8lzRdR"
"Xa3I+76LSPqZee5pDH77a0RCoWbMIx0Kyc9vZ+WfnsvIL58ijENGG6MkVY+3v/udDEZHqDeSKWCH"
"rmtEpBkeK7P+K3fC3gRVaAUMujYMmQyy2IKZ0HhoMLFzmJRFiIQ4LvObctcdl4kOvCwYxWR/gsUi"
"S1msTZmxqo/TrjmTvU/uQaaSk16zjqtffzatHQlkR2nNN+jOazoLhp5WRSEDs2bN5KK5ZzGxvcbT"
"33yII+v7Gd+4k3V//jIGNw0T7RiFUit6eJx4ww5aLj2f8NHtqEYdO68Dqgnp7IVw813YsQptb3sD"
"6QO7SFacz4L3vQf7k1v45b98nWdVkYNrTubUl5zCrDWLuCvwuPXpjdz8xZs4aWCEle99JU/vriM2"
"b6b9NZdT+eUmGBnBLltOsncQ1VFAPPh9ROMgreesYfDrXyUJBXTOxgw2yCzuYNknzmH3P/2UIz8d"
"pLptgpdffCU3vOZVyFkCTzTobAloy1oyfoWMHEbZMl2e5OR16xBtbQxvPoI2MWe/+QpqYZ1waAIv"
"52OjaKqvSjbxM6mOToyT5XtZjJZHYbGeQOQkIk4pvmUBH3nvm+k9bTZP3P8k3vk5Nu7aQFvrXJYu"
"WUdbyxzaWxbSlZtLV3YeMwoLCWoFrK849w0vpdZq2L9nhMa4ZuzxHaz40zNpHKkjwpjei+fSviTD"
"6F1bKZ5/Fo0ntuNpsN15VMcM9HP7KC1bgd1wkKo3myXvfBfR33+KXRuewb/6LeRfegmLV8zhnXNn"
"clZ3F35XN9VFKxjo7OTR738f+cjjdH70rRx4dDfe4H6CpQuJyhW8VSuxA2Oo9bdCfJiO889k/Lvf"
"pPvc+ZRWL4WJiJaV7Sz6o7PY++V7qBxMkX1FLv/QVbznD99JP1Vag3Z6OufRWZxJR8sc2gszaSnO"
"oa11Lrsqwzyw4UkaPTlqu8q8//Pv5ep3vJKfjz1M8tBBbJBHpLpZWWsmOtDHBdk5PkSHFdOMugOJ"
"mVDj90hGDh7hfav/jE898pdc9bmXcuvHbqLY1sv22g4OlQdJZYjK+cisQgqJtBaRUUwcGmXR7EWs"
"uGAed38jhvmdjG0/yOa/uZsV164iOqxoW5BjeIckru1BPfgopfPPpvbkY6ixKixZg2pvxwsKjG/f"
"R9+H3of/na+yY2Q33sveS9q9kDgrKCpYICGbJsRhnf1hBa93Lt7b3s1TX/47Vt3473S/+zqGP/pp"
"Cud1IWd0YNMI8dRPsP4IbWefwcSPbiKpR5SWzqJjaRt6YZHCjBbWf/EuymMalnfAWJmlZxb56pc/"
"w7Oj+2if14EMPKwEiUQYRVSPyUz4lA+PwuYJhscPctk/vJZTV5zNB077EOElGtGmMNXEQX11M335"
"W/ShHZ+ABcd2X1sgTBA9JfzRAjWd8rGLPsnV37uIUk8HC1+1jKvfdTqbx4ZYk5nFAT1Iiy9oocCe"
"9Ahnti9nx/YDfPG7P6XYdylLVs/h1LPWYqShum+EZ27bhsrk2fC1ZwGDmjWXxsABxIOPkT9lDdX1"
"j6Hufxa/tZXGhu34i06ne/gIWzbeg7r4beT65lE6Zy5hawEKHt/zLcpqtrUVMMqn5I2RHMigX3Yt"
"u2/9LvNXn83IqWeQ7tyC19KJuf37GDNA67qzmPjxt4nqId68pWz/4rPgQ2FNH7WxZ7ng2guZdfJc"
"bKp5+IHH2TW4g7vLj/HON17F+XNX8rPGU/TaApGR1HREr+jimdouzms9jXu+J9nx7fXEBcEHL//v"
"hNUcwbgm7crAaIzxJEocyynwQk3w/9sCttOzZALQhvzMEnGljtlTQeZXIpaXqWn47rv+E1Grs2Hj"
"IUZ+ali+uJvMKRbfCzBRChqk8dg7PsLOyhiRFYyYiB2Pb2bv7kMU8hlMopkYrhOYPKqvw4EKGxrV"
"MpNG+QD+k4Lc3BU0vnsftHcgMjPouG4No5t/STJ7ESI/l5Vn9TF3VguJtrwsI5iHTyIsi4Wloyho"
"FLqoG83TI0OYnrlE99xN29nnM/539yKHNmC7LJlTV1P5yXdJqxVk+yxMJUH2tiEVJJUaGM3Ttz/G"
"s3c/SSMKiaIGc67vJZKCPYPjzG0ZJSkmCBKkLdDIV4iSBmO7Kzy9dQsHd46jNk1w37t+CIVuZFcO"
"lbSjj2xDZSTtC2cwvvXIlHLZF6ST+J10Nrg42Bo5tZPS1GI8Dz08QnJwD4vm9zC/t4Viu0JXDbq1"
"jfFyyu6f7eKu7ZuxnibTKljcupjnduxhywO7uWDGYi5avJLeniKZ2T109M5m10A/QW8H577qUg6V"
"qsg5JejMI9rykC+iZnShxw+iRiJk+xzS0SqyZS4z155Bfe+DNBauJVi2isMLOilmPN5TCLjaCzhJ"
"eixTktVK0iYs9zVinsLiR2C0JbP3AKWVaxh96jmk0ORWLiR++hZMKY86+QzkzDbEkm68kztRp7Rh"
"FhU45bLTGRydoJrWWX3eaSy5YBnnrZvHxX3z6Etb+M9n7mGwS7Oyeykj44M88cQ2fvTDp9j/oxEm"
"aj7eQfD2j9C5rIuujoAWKZh49Bl0dQJyrcRjoeu/muIB0eimV/27NdGTJDjTiDFMI3UF/3wX2U6f"
"uX92HvHYINFIgIgjjPbxRABpB3Z/gSe/eYCNCw6wvnMvQxODXH9GgZefuYk6MYPVmPK8Vhr78jyz"
"p0ih2MK2R58mHRyF2JIORUidgRB0HCJED2H1CB7gd7Vj81k8YUnTDIXZ86gUfbKeYrkvOU35tAmP"
"pLk1i0KyxhOszCQ85/k02vL4s+fT2LKbTJxCRwt+MkTy9J3ouAZqBnr3EDYnYcwj3RPidXtAyJ59"
"Y7R0dxBn6lz93gK2c4K56g7mr/DpoMjIg2N8694qP9AVdu8rkwyCJ3tBV7AmwEQGf36R7Gt7yeU8"
"WuYup/zhkIkNe5AyQ1qpOO/ZU1OFnhN3Bv8KhEdiaglWBNg4z75HNlEzo4TlGo2hYeSEwDQC7IEx"
"xMAYKqmQPBuzJd0NXsKeM/p4iBiJTyXRPLKjn9pIlhltKzhlwXJu+8EPCObNxEYxXdcupp6USfvL"
"iArYfRbb305yaBB5aACVn43WGj9XoJRvpZJaWoAWYyk0m0GmCJIElDB0WUu7EcSpJih4qJYAnYI0"
"MfHwQ5gOiVy4AG9BJ2ImUFB0dHfgj/jsu3svwYIexrb2c83Z67jrkbu4e+dGcuUYVgoGqRIwwZ6x"
"CSr/MMoWLZGiiMj4WL8KVQ1Wovo16XCN6rNDNNp9wr2bSYZGQeYxtRq5+fMw9ZBk6DBS+Rwvd7t3"
"XBKd4rFqEqwkCX4ui5/LkMbQ4Z9JVzTKiBxnoLSD6sOP47WfjGxdCINV8FsgSBElD7yEHQ+0Udkv"
"ETmBF+Xpt1XSCRh5fBMHhw/gLZ1BXE+Y8cZTMLv6Se4fg3qCHauR9JfJz+hGXbiOxs5dpLUKsXaY"
"7MjTtESSFi2oYhgWhhko9GSAZ2FEaxJtKESG1omIam2UbJCikwRbrSGD5ZSWryTs30C0dx/+jDZE"
"PmWo0s+c157GwuvXsvtHT+HN7+aWn92GODjOzoPLKVR91CZJkqmRVg0HNvoEthWFR1rVMG6xGiQF"
"zB0jmNpuMqeeSptZQ44cOjMPk+x17AMCTEOj63VnKS3TeEFOhIlutjgKC6rkkZlbguGUYnsrg9t2"
"8+RPdmCynQhdQIz0YZHI1ha0LGHrDWyUwVigLJBezJZvVNlCBIUi2EN4tgINhZ3XivSzaJsy64Yz"
"0M/sZuh7W8iuOQW9ey/JcJ2uFSvJdc/igLcPdUEe7jhMrVFHFIokySGKZj6d+2sczCsezEV0e5Lu"
"Jn77iE15Io04UI3xhyrIwxHUDqI8SzQ4CKND2GAmE1tDll39FgZ+8RPGNwzhze1Czi6w+zOPs+KP"
"1rHk6lXsuPFpkBlsJs/Yp7Yxlg3ZZVrBy7v6scyivCxpKiHxQcf4uSIiOxubGkzNElUXsHtDm0Nj"
"hs9hRwcQqguAZOhIk+RFTTWknZj2USHwVdZBwIwlv7CTK/7yjey451nGdgyiMgEceIRgfDtm98MU"
"OwyyWCIdOIDK5bHlAYJilmJriUAJlO8T5AKUB0GgIOzHGEv2pYuY+ca1tC9up+O8eUQP7GLw+8/h"
"z+zDbh4hGR9lxTlr6V48l60Dj3D6DW9kPKfh4CGigRyFdaczduBezEmnUjwQk7Zl2ecpRjwok7LL"
"aO5JE+6fiNi1Z4yRzf1MpGXsE78gt3AdE49uAj8ic9lLWXVSL1vufJhTPnwD0d59VHceRGSLyI42"
"Bm/dSMuqNua/ci0zlnfTelYP47sPko5FZDyPICjhBTly2QK+lyWXyeD50p2jxoIN0aM78GbNwlej"
"mI23og4+iTnyDMKUUMIHT7tO2SZJm+tq0BiiE+NF+zLrGr+lQJdDdj6yibi/jsQDa/HyvSTViNzS"
"5cx8zZWUn96GCDIkA88hcj7CanRtnCQcQ4chKEF8ZAt6oB9dtdhIY21Esm+Y+i/3MnH7VsqbBhG9"
"M5AHDUl5hNOuu4yFlyzi3l23csOff5ZF51/Fk3ufoZBNqd3xJMXTLybfX2G8vImgewXjowkVY9iD"
"4MFqyF21kPvHI9Y/N8iqRDMQlyk//CP8CY+kYynJz36CPGM18ZwVvOuTN9DTmuWOXzzAuj9/N3bn"
"fsY27UXlCtiuFsbv2k51+yEmNg1QfvoA+nCIPtJAl+ukgwdJa2OYbA5dHietlknDCoYULSrY4R34"
"bX1Io+j5wFuJhxvERw4SLDoHVBZTqyJS0+z3mkZWg8bY6MRosCedBlsEpJpkrAYi41CVAtAWf0YP"
"PTdcTP+Nd+MV5mKqh1EdM/CKM1DdCzGZLLKtG9nWjQ3LiJxFtM3GzllKaclcCu3dVHZECK8D1TWD"
"VVdfCPsaTOzaxdo3vJTe6xdw64Ybecc7PsMVK6/krsZWtv78djJzu0geeYbqzmFmvOw6kqeeo1HY"
"z4TfyfIRxbV9Jdo09KWWlbWQfQ9tZsu+g1Tuvh11aATv/EsJv/djhD2Ct+oM0kNj9Jy3mpeffTJF"
"q/jZQxs4/S9/D7t1OyNPbGXeeacy54K1jB+OwLSQJh34na3MnTOH2BSxLa0olSL8Al7vUmyhhCm0"
"oLpnIRQUizNJa0eQXSdRe+pZWt90BY3n9iFkN6Y2gfHr2KxARke7Cx02K22iKk+ABiuZcQK2IDJ5"
"VMsMbBwigqzrbm/PILN5wr11lJ1FdOBBTH2coLWVxt6NpEe2kY6OocMB0j3rSccG6TjjFOIjZcxo"
"BWoTmIYmOTKKNXXsyATJ/jLDu3dz9hsvo/udi/np/f/OH1z1t1y4/CWEcYMf+4OM3XonYixCZ8A+"
"+CQVIei96nrUyBYqB+5hb6Wfp7YeYHjrYXZt2cMjDz/F4I5fkm64nSDbRf6666h9/+fYTXciVq1B"
"hpJUWNrOPIvTZmToWLmIvqEa//ntn3Pqp96H2LmXw49sIK0N0hg8QjQyAtVxzMgINkloDI4isHSu"
"XEz56SdID2/F2Ar68D7M/o2YoYN42Rai4X50/QgydxLRrr2o7tmkGx9FRw1W/+ml9J4+j8P3b8VT"
"/lSb/Qkz0Y58JdskK7PYbBHZNgM93o9OUwegCxuYaoQV80kO3YEOQ/Kds+lbvZjRXQfxvAKdJy9D"
"zWkjPpQivHZaenuYf9JKhp/djQ41adYjP6uNVS9bw+Ft+6gdGeTct1xB+weX8dM7vs0fXfwJ1q04"
"j1pa4dsP38KmnIDBA+jbNqIWzIL6BGbzZqpHBphxxjXM6V6Kz0FGR7ZzZN9Ghg+tJxraQ2t7kdaX"
"vxq7fA3Vr34P1v8C0duHal9GsmkL6qwLOVgJKB0contlL/6qBcwfqfLjG+/mpM99ELZs5tADT2Kk"
"YtW1pzLeP0pcjQm1QGmPU684g7HDg9QHLYgCHS9ZTCbbSjpcx1jBwsvPpjpQIS5XsPXdCLWcdP9G"
"SFOE8AmrDco7jpAMNKYx9Npm4198YjQ48HKOpwqJjeqY8SNY5aMyWRejRQlGD2DKmxAijxBF/IxH"
"psOnvLdCvtjDBe97Jd7MVvof2IIsFals38bA9p3YwIO0gSjmSRsNytsPEo/VOPcd19L+vmXc/sNv"
"8kcX/xlLVq5iIq7xzQdu4aEnfkxhPCRz9jqi+hjmiT3Irg5sZRy9YyejG57Bn9XL7BUv5eTV53PS"
"2jPpW3cBHaddRGbOKsYe20T1S1+FnY8jim14nSswQyOIC16KWrka88SzrH98L61BACt7GV+zhJNr"
"MXf+272c9ne/j9p7kJEN24irIWGtgQg8REVj4pjDG56hsu8wXr4NExmWv+kMFsxbwZH1/S6kXNZD"
"Zf8RdJhFmBBT2YgJhxCZTqTKUd8/SNRfx6Kx1iCn4LOOz/KEJjosjisK6YOBNDFg6u4hbCsiCByx"
"WVzFb19CcfY6YDOq4DF6oMpgoUywrI94zziqswvbSLHKR+Tz2MQilKRWLvOS91+L/67Z3P69r/OH"
"L/0o3SvmM14d4fsP3sOTm36JJ0tMbNpA60iNrtdfz/h529F3PYYo+Ihdw5g9h9n3lX9mX+/N5HuX"
"kG3vwqYQDw/Q2L0NOzLkSE665yBnn4xZuBDvNdegtE9yy13Y4RqqpYUffushLqrFyLeewejrL+Fl"
"gccdH/sPzv7cx/ECwZZ/vxU1q8txcpgmzQPt+F0eyWiEv6CdkbExzGgWchkIDZV0NVoOg92H6l6G"
"rVewVcf3Za1CKQ+EJd8xC5tEhBPD01huf3Oc5W/+XiHJeq1Y42EQaCwCiTaaOdecQc/qRTz9jz9C"
"1XB0DdogMwnGNjCJRNoMoCnOX0nSWqTjZW2M/GgH8cFxrDEIXyFLAXiKtDHGZR9+A5k39HDXD7/P"
"H1z8QfzFbcg6/Pi++3lm2xN42sNEKTYy2IkJgqCD/DWvI105j3jjDjjcj7jxLqK7H8XLd2JsgnEt"
"+0jfB+ljqyNkznkL8vJXkc7pJbdsHjzwKJWfPeJQmMUSiACRy5NGhjOvOYPxt59FqCRzbn6IR35y"
"H+d/5q0c/shn2P6DW/FmtGHqTX6OKEUqgeou0faK+YzfMYZnAhr7nsUmCfgWoTVCtbmYP2ogrMbq"
"BOllXLNIWmflOVcw2r+PgT2b8JWPNg1SU/ndg+4mvWhHNHq02xCr6ThrBa0L5nDwnqeQWmK1xqYR"
"qqsd4UtsNUJkilgk0eA2PJHDm99Bsm+MZLCBzElEVoLw0dUJrvjT68m8roe7v/Md/vDit1HtKlMb"
"KXPHPb9kw/an8WyACbXj2mjECBOQjoyh+g9B/yjJto34W/YQ3/E0QWYe+a4+ovJ+ZKEVVWrD1kYJ"
"uuYjczNI+vcgbAmzdxh/eJzksQ0kQ6PIQgFpHSmbTVNkIDjw5H46x2Pq82dyeO5CVscpj331Fub9"
"xbsoHBli+JH1yNYC1mpHqFZN8To8VHeRaPMhwv1PIcij8u0QVhAyg04UJhlDWMfh5XU4hGoaNpBC"
"MrB/G/Wx4SZNswFxAp0sT2aaVL9iCkaCkIxt2MfBnz2MsAHohGD5HPyeLpLdR7CJRKpCsxLio1oL"
"yF5DY/1eop0DBN0lvKyPRaKrVV72kesIrmvl3pv+kw9d+hYmuoaxdcO99z7Lpm2b8ESAaSQQaQhT"
"iC1U6uSWzMfO66IxcpDsQIP4tqfJ6i7ynT1U69sBD1FsRZRmQLWOlhPk2ucgtE+8ewN+zxwaUQnv"
"tFPxbUJ64DAo/yjdd2JQyjC0YQ89ZUvY18fIgmWcJA3rv/R9+j7+AXLj44w9vgFVyiEMyHyO5MAh"
"0sFh/Jk+1D3QGaTygQxGJ8y5ZAGnvPMs9j29Dxm7rgubxK5lxcsghWhyZU1yTLtcwQkUsDxKeiZA"
"eR5Ca2afu8hZprFxsqtXkVmygMbG5/Db8g5paQU2jpjz5quo7zxEtHs/snUGflsOv6VA2D/IVX/1"
"euQVHvfceAu/94o3MtxxGFvPcP/929i8dSueyKCrKaKRYKMUGRnMRI384rnY2e3Eo2VyRyLqtz9N"
"sbSU1pZOhnc/hJwxA+F7GJnFy7QgvRTaMsT7t1KYtxxyM0i2PExu9lzCqIA8eSWB1MR7+pG+5/gx"
"DZAkSF8yumEPXSYm6Z3JcOd8VnUV2fD3NzH7o++gLYkYfuhxvBldiJyPraeY8QH8oMjMi1/K2MbN"
"jnrRppg0ZsZVC+h9zzL2/WALdszRFQoMIqOcldSaScLlSQFrG54oATsTfZTVzmLSEKGyGAvJeB2D"
"R7L3II2tO5C5HDJo1o8zHtKGVNc/QzxSR7b3QaAwsSU8Msi1n3oL+iL45Y/v5v1XvoH9/nZUNc8D"
"d25ly6YtKBlAqKGRYuMUmYKeaFBatQDmdRMOjpDfV6Xx8/V0LD6NXKo4sucxhGrBm9kKEZh8CdHa"
"ij40SNDXjR5LCMd2UZq9CNuxiOjxX5DtmUFEN8VzTiMfTVDftg+ZU1gdI3SKjROEZyk/t5dCGJJ0"
"9TDWPo8lM7Ns+ccbmfWH7ySf1hh54FG3vEIigxbS0SGqW58FrSAVyEAilMfwk4fZ8e9PIysSghRa"
"G8hEO2fTOiBWc8JzcxTBCTXRRwVsrUHlW2lZdTbhkQOE46HjjPQloqDwerII5WEmEkyc0PKBGzAz"
"Oomf3YGa0eqcKiHQlTKv+du3EZ0Z8cgP7uTtr3gNu802co0iv7xzG9s3bnVAg9BAqBFhikgsulJj"
"/ulr8ZbNptw/SPFgTO2O9XScfj75ckz/E3fj9yzCSoXIS2zsQaGAaC1hR8eg5GrLflsnteceorhg"
"MfSeTPTwbRTnzmIi6aR4/lpa0gqVrQeQvqNZxBhHzxgoatv2UkojdHsnQ2IGS5b3sPnzN9L3rjfT"
"4mtGHnsKWcg5vjQviwljsi+5jGDdqSTPbXPkU0IgQ4PO1jEnCWa+ZgnlsYN4dYVtSEeuJpU7Kk68"
"gDOOALLZLyOkjwryxKOHkUHgWJR8AS05bK4VvXAFNp9HiIYjzh4cxkYh+B5S+KSVCq/9zPWML5zg"
"6Vvv561Xv4Yttc3kkhKP3rWbnVt3Os2NDEQWUotMNXp4nFPPfQlzT13K0MAhCntCxm57khkvu4Js"
"/wQHHvoFQhXJti/ApCFtM7ux2pIECtHSgjg8Qtey+TQGyxTbO4kGBgj3b6Jt6SL8xedQu/tmCjNn"
"MCZm03HpGbQ0RhnfuA+ZVZCkrliQaqQfUNu+j1zYwHbOYCzsZN7q2Wz/0neYc/31tGQNw088gczl"
"QUsEHiLIIxKNOTzoeLKEQWQ0mdM9XvnfX8snXvsnPN12mPLm3WS711I64xJq259CqskmtBRj4hOU"
"ixbZKUIBIxUmjUjKo6hSByauYzMSkfOgdyb2tHO56r3vxs5fyGi1jN74HIyMIHIeIjLosQne8Dc3"
"MDRzhI23PcqrXnEV6yc20WrbePyObezZuQ9FBl1LHOlnCsoI0okyF176Cpadvood/bsJdlXZf8vD"
"9Fx4Gdl9Q+y7++cIUcQaSXbGLAwJ8WCVNEkRC3pR3S2Yw8OoWkI0USXT0UFUroM11Hc9Q9uSubDi"
"Iuo/+w6tC2YzkM6n/coz6EnKDD+zC+GrqRYbG0V4WUV92z5yRNiuXkbKbSy4YCnbPv8NZr/q1bS0"
"eAw/+RiqUED6GdL+AdIDh1AtGWQpwNRjZCApnt7ONVdey6v6LuXBA8+w57HN6IEAUzfo4YNNslb7"
"f6rYgKPgtRqvawFeqRtb3Y/IZhF5n+yp53DV772fWy48lVlLFvGzwSr6yEGI65AadHWMt/7du9mb"
"O8SuX27i2quv5JH9j9JhWtlwz172bz+ALzLoSCNSi9AWD0gnylx22ZWcfO4atvXvJn1ukOd+eB8z"
"L7kcseMA+++/G0SJnpWrIJ+h2kjxggxJo4b2JRe/5bVcefHFPHL/gyRjZVR7jqRhMQIynUV0OaW2"
"40lKy+Yhl11K7T+/TvvSBRzJLKHnpafQ1RhjaMMepC9co5w12DiGjE+06wC+riNmzmFipEjfRUvZ"
"+U/fZf6115DxE0af3YAIMo6iUIop2kYS10vXGJ/g4QMP88v4OX7x9zehNxvM4ATJ0H6kDACNEiCE"
"OS5c9HGC7jJHB24IiRAepjpKOn4Imc0hPQHWI23PYbtm0dI7i9u37WPDL+/HDGxHNBroiTFu+Mv3"
"csAeYu9Dm3jVtVfx4O7H6KCNLQ8f4uC+w/h+gI5SpDZgLJ6S6CTk4ksu55TzTmVD/1bM1lGe+OHd"
"zLj4MtRz+zj4yAOooBORzRBLn7gSs2T1UtIwpBFp/EKOwcFBtj63ibB/EBEoPL/E0hVLGNy7H6ub"
"MyJSj/qWR2hZ0gcLL6Rx89eZuWo5e8Zn0nvWSfSYMQa37HHJEuuIR4UFkfVJ9uwnQw3T00t9PMeM"
"l61i2z9/m3mveTXZHIw9+wyqWHJclrF2R09qUCUfGUns9pD+HQcRzwGNwLHdK4XA8Vg6Wof0BApY"
"OQFPjcwRzTKhVNgUdJBi8s6tHx45yO0HR3j64fuxe9YjwmH0+Ag3/Pl72Ll/F3s2b+GNN1zHHU/c"
"T31c0z9Yp0xKqbOVbLFArlgi214iKBXwCgWKre0sOWUZBypHsJtHeeg/7qLvFVfC07s4+NiDCNmO"
"bC/SduY8avsmEFKy5txzmKiUqZTLSOXTODxAbc8hpFTIQoFM0MJJq5ewa8t2hFJ4nUVEFEKiqG99"
"jJalc5DzL2Di+1+ia81K9g3NhFIbteEUU5qFLfZiS/OwhbnYwkxE9wKSCQ9SSNOYyLTSc/Vadv/T"
"Tcy59pVkTMjY5k34xTZM6tZJdQbYNEXUNSZUpEc0NlbOgzYWo7VrOmvSCh9vZ8NxkZHmgja0Vk1S"
"H4mxBiEVWIW2Ies+eDZ9yxZw25fuQOZakK3tGB1h4zocHuCt730jRw4dYmf/Tt76wTfzk4fupVpL"
"MInAZiXKVxDhSMJDSLVFW4NnJSbVdM3vwj9Q5uHbH6bn4ksRT27nwGMPIkU7NuejZvQRZBQWQTxe"
"Q1drkPVQuSKmESJ9hQwkRlts0iQnalRRhaw7H0dqiIyGagXSCGsq9Fz5JkKxisajP6PlbR+jHHWh"
"Du10jPASNwhEeKAd4kv4HjZKHDeJkPjzu2lZLBn63JdY9fqXcujWmxh5ej3Sy6PjGNXuwUSIiRNU"
"voCNtbPZTXSdyjiCcR0mKGlBRETJxIkRcD7TRpo6AQvpIQsF0krFDbAwCSe/7Qy6FvRx73fvRhWL"
"4AXYKEKbiM40YN2FZ/Lzf/06q19+HmmjQRhplFTo1GK0ayY3RmOaM+cciZxxFH4CPC0ZTBP6Ljof"
"/cCzHH76UaRshUwW0dIORmJSkJ7EyiYjjRSYjEdwUhfpjgG8fAvpYBWBcfzUYXP2SSZEtkOy1yDS"
"FNuoIdIQbSt0vfx6IlZQfexeMrO7ENo2n8mx8Fms4wqzxjHRp4mbrIIibSSI9k44uBuRnUnHmbMZ"
"+dFXId/jVj9KEMWA3Pwu6jsGIXSE4EiBSWNmLp2DMZahnQcIfAUiJIxOgICFkGSDVrR2GoKQkMmh"
"Gw2EcONwUuEak2VnAdHjQiqr3cOyYx/ZxYs4+XWXkyYpJk1JgTQVRPUY04jdbCJLk2NZI2WT7j9J"
"QRtkIPE7Whh/4BEGnn0WKVogEyBKLVjt2Nlzs4tEYwk2dEOubBrRcuUKrv/kR/nBJ/6BoR9twu/K"
"YEKHkRBAWo7ou6bI4rd28+A7t8KgdHW2Rhl0hDEV2s+9ksy8cxATY64gYN2ADSskynNTYIzVWG3R"
"UexG3ikJ0mK1xvMzhEnK2GPfg0aE9VtQvsI0IrJLZ7PsA9ex4c+/BsNjCOnuC7o54sBlsjwlQJ4g"
"AYMkl2kKuJlh0dq6IFy4yWU2dbODyPvQ04LNObNoI4Hdux/COsWu2eA5DBeySU9gLXJqephp8kE1"
"WeXt0VGvQioao2UajSpCFBDZPCJfcg3pkSFYluelPziP9X+/hUPfOIxqDdxkz66A1nPnUl0/gD4Q"
"Ij0wkcZq7djnPIEsaPK9guqeGBE6an5rLDTKiDTG2DrZtg68Yqvj6rQGIVzxTinhihIARmOmpqRO"
"I/8Vlmh4EB1HCK8Xi4fAYkjB04iWdkQlAW3caAAdgnFzpqx0Ew+V5MQJWCDJZtwZrEWzUw6FaQ55"
"wssilyx2Rf+9e93YhZyBvIcNJCKsYY8MYl6Qjl68wCPJY3DYR+fj+QjhuJXJFcDz3G9EIFslM1/d"
"wcRzdepbUnTdYGsh7af3UhuvkRyoIXM+xAKTpsg2H+UpiDx0rY5uxKgWNzSj4+Ruhh8dAKkhroCO"
"MaYOpNP4a4QbQZBoJqfMHEsm+TxqfoQrP3qdeJk24toIfa9aSUtfB9v/+QGkzGJEU0l0gj+zB9uI"
"MONjoAQusoppROUT1T7aJCTVKXMvOI2RHQeYODyE9HxSGTDr0x8memY7Ax/7FNIXKAqk4xEikNhs"
"BtHVjQxdQ7NpRLSunkXQ2sbQQzuRQeBMXooj65xisRfIrERIMKFx/58NIJPDxhaR4DRNgJlIOfhP"
"/chSBlkIwFpy89up7BrHJI7z0jYpIEViaV/TAxaG7zyAKihU3nfRQD1l+MlRN0YRCV4L1rrBIk5S"
"amquoKMc1M3z106N1RM6cZkqOc0CeQEyKGBDg0ljhNWQCvfy3NS2qTkWRqPmzcOGdfT6IYQI4AWG"
"j/1Onaxs0I4xjsJB5bKYKMFaiZHOk/Tnz0fX65iBw5DJYOIIqZqTSKyFjES2FEGCLk+w8HVn0dLT"
"yfq/uw1ZKGC1dGlAo8ETWNGc5hK4GFVog65p5lzYi7SW/Q8fQWUz6CRBSInKeZioOb1UKKSyBK1Z"
"kvEQY1JH+Y/AalCej04b2NQivaB5XmtUxjLr9DkceHA/k6RgXlEgA0U00HCTZabPDjbSHS8mbk5D"
"EVityXRl8fIBtX1jSJVxNWLV9LZN4rTZy6CTCliLCnJNSmLrzm1rITVN0jkLuLlREBPGv3nB//g4"
"Ovw2J2CsKyxIR+FvldvNNondeRxk3GQaUqSfcQgG60Z3IAQEAUjh8tZG41sfbS06FW5DAMIXeEhE"
"RmESjY4d+Fv5PirjhljpFCzaJQJSgRHqqNftHFqklkgfrBII6TunzxqMNljtxvLRnMgtlYfyBEFO"
"UB9uuFAwNWRnevgdPpUNI44Nxx7lkbLGTE0wFZOi1wmZ7jwq59PYNwYqcNqd1LEicOsmDML3kF4e"
"E1YwRk+ZcWstwnP3EkY7h01YdwYTEyWVE0TC4rdhjMLY5/Pf+e70kQJjNaqQx9ZiNwcpm8fW6mir"
"mw5Zc2SOl8XU9NGzLOshswLSFOsJpJdxlL/aVVAo+m6hapHr05EeeMKdiU1nj0CisgqTJm7xw8Tx"
"aknZNJWO+hgFZBSeLzBpiI1de6apN0EEJoGSj/TAagtxgtUJKpvFNkfhmDhEeN60IVaOBA5hXRtR"
"kmCNQXp+08MwqLZeTH0UG9bc+7I5TGJAR0xOEjdZg9cjsP3OfFurkRiEMCgpgIQoqf7GdMLH1QBu"
"pyiEzbGOkbBYP+tQCGEVXXGEmsoPiCeqztXPufkLNnZBrsx38oE/fQclXyKkz3d+9CAHDm1EkqDr"
"Aq0KvOH3X8Wi2R1I4Pt3PsrQwSHedf0rSJtDq4RwkBpjoVyNuefx59iz4Tm8wPFavvadr2dh3wxS"
"KxCBj7CGqBGxt3+Uh5/ZxtDWncjAR2VCdDXm8ldfy7nrlhGm8K9f+wHDh7YhhcD6HjLjoRODFAp8"
"i792KWZzP7YWOdooo2g57yIaB/ei9+9ABoGb/tLsZrQ6xsvmkXENTQVw6Vhr4+bQSotNNV2nzuDa"
"//Fqvv373yDe0UD6CrQ57q7C4xbw1Nh5K49Retd4biAOmxvAkXjrRkhSGQLySM9DCoEQHpoUKRXp"
"RJ2T1p7Cu1+2BoDzLz+bl13+YYLcGNqkXHjlpXznb9/XbNaAb936KMuWLeTTH3nDr33GamL4yOdv"
"5itf+DeQCR98x6s5Z+XcF3zv4UrIP3/nHj7z2W/iiQrp+ABvfs3lXH/V2QDcescjDO3ZishlEMIJ"
"SmaaY2JTjcoWsCrAitTljP0AO1Fz8778LCZJ8dtLSKMxJiGdSIkOPoeSHgjfrZkxeLkCJnbVLCmh"
"ur/MPV//BXokwUoLxjTXTrhgwogT1+GvVLYZshwd8ixUZtpQZ4fREoBqDzjnTZczUa8RDY9Dah3j"
"ulBYYxEm5ecP7OOSV55Pd9FneW8nOxuG9fc+Q27GLG762p/QWXJW4R2f+jb3f+OHLFqzjNddfT61"
"NOFINWT30DiHJ2rUsBSyPp6yXHrOKn74wCZG9hzgNa++iPkzOyknMeOxZiJKiZUkLwXZjOSydUvp"
"XrKYn/zsMdAhBsmRsSp3PLKJu3/+AGF9wmlaHGBiiYk8TOphlYfdO+JiZT9AZooYssQHB9ATEdbz"
"UF5AGgvSiRAdaoTnijNM8j0LV8AXRmPSBOkpRMYnGQsZ3XQEkRzNEUyiORxXqGnWg+3vXoMnx7U7"
"R8AJc4r7vdmaqZQgiRJmnbacf/nK13nrH76Np7bsxcvksYnzJI21qCBDcngXf/iJm7j33/+A0Gg+"
"+YFX8v3v3MebbriEM+Z1Y4Afr9/Dt778fURbweVzPEUbis9/5y4+/bGvEnS0k8kU+Nxn38vrz19F"
"HssZaxaz/c5HUErheYo4SbjiLX/NkX2HKXTO4JVXX8DH3nEJPgnvv+I0Hrj+Kv7jH7/Gg79cz3M7"
"j5BGdcrj48ggwHjtLF23hlVL+2gtFhiaaPDQQ09R3rcbmY2RKosOs3QsXcwlF5xKxvN45Jnt7Hps"
"PSvWzCOT8Tm47xAj+7ciqbuZwG5OXBN8bPFbWxykN/ARYYTnZ9DWOCaiaWsvfst57fY3eQkhre+1"
"W9/rcS/VYz3VY6XqscqbaZXX615+r1Wqz/rt82zflRdYr2++lbLPqmCBld4cq7ILrQwWWpU72fqd"
"51kKl9hPfOdBa621iTX2X+/fYjceGbdaGztQD+2cl/w3KzvOtfRcYs973cdtaN31x1/4joWVlq6L"
"LJxj3/rX/2Enr1d+5IuW3Hn2jkc3WWutPVSt2Y6TrrPk11o6zrNkL7Fv+dR/Wm2t1drYB7cftrRc"
"Zj/9tR9ba62tWWuXvuQ9ltKF9qZ7NtjnXzsHyvbCN3/a0nqJpedK+/IPfMnuHalM/X/DWPvHX7rD"
"bhsoW2ut/eCnv2dhpQ0yC63y5lml5lql5jRffTbXu8oqv89K0WOlmmGlmmml6rGe6m6+Oq3vt9sg"
"6LCB32KFkPY3ldtvSeFgjzmFJyexCNmcgStBT8Qcvm0LQmRdStKkbuK3dsObrY4wcYhSlk9//Gu8"
"7OxlnDq3g7dcsJzYpkgh+KO/+z4HHn6UzIwSUcUN45K4AVknL1/E5a97HaqYp6u3mz9++6UAbOgf"
"48F7n4Ccj06boYexBJkMoqVItsXFxf/xnZ/yZ++6lCVdJRbP7caf1UNca2CNpaFTlFIQxTQSQznR"
"PL51Px3tJVbM7mBRTwtf+/wHWPnLrSxcNo//+Pz7yAuIgO2DZdoKAZ9532WMhZGDC1s34cwK6Tz0"
"6SsoLHH/UDOh0gwjm2nPqZkN4iiF84nlyZqedJs0G1PEhrLJXRk3m8UDpOc1K0OimUiIEX6uORjZ"
"YuMaquAT7d3OH/zpv3Hfd/+IShTSnslyyxM7+NZnvoHfmnExonXxqwJqRvP6S8/g+kvPmHoig+G5"
"/nGue+unGd21H3JZl+FiMiMGVhuSeoxVHtHwIPsODLC0q0Qh49FaKpKmbsaw1BJtJCIb8Hvv+Wve"
"G2RJh8dB+vz53/8+H3/zS1jQXmD+igVcd+1ZFAU0rOVT37yHz37mRnK5DH/z6XfxjpevdWvVfA7p"
"+aAymMa4K9BY47pEehUShRi0rhcJi/jtOcB/NyQsx3jRk2PX0oh5Vy8k11Zi+/c2IU3gUIHC4pW6"
"8IothIMHEMJzWR/bjDmtZO1JfRzAciRJyYiIlt42Zs6eyeDAfvzm+ABpDHuwjJsQWTdUKjFWCJSf"
"oberiOjK8blPv4v3fbDGoc1bqGPYDYwl2lH0aoONkub83wyhsewGkubPtRAcAcrWUfFbA/NnzeDa"
"117CgkV97N7TT/9IhQNW46HoKipmz+1gD7B/cJTP/tW/EI0OEEWaT37yJs65+GRasxnCNAFSl2Qx"
"CXgBUihsXMd6Ia/+izdTLTe446M/wFM5tNDPO2vFC/ClnBABP58NbYq1w83nUJrFZyylu72dbf+x"
"HoGPKvqYWgO/vYfMzNk0Dm9DeEVoTvdKajFrLruCV/6313HXkZB5rTn2jCbk27r5o7/9Az58/Ydc"
"fTRJMVHCtroh8gv84hu38O+f+Bp+VxtowZXXX8ab//vr6Fi7hLd/6A381fUfYTwxbExBNzTSNPsi"
"paNhau2YQdDXw1ORxivXKR8aJASetuBHlrAes/ayi/iHmz6Gn5UUgEuaIdujlQlmFFsQccpYmPKs"
"scQ1jQrDZuLGQGWUPTWXyWukTeBV02MG47iupATt8cQtj7lRPcJz4RJ2igR80oO2U8ehOS6ujt+e"
"RskeS75jjEbJHPd87h6kJ/BEAU1CkM1iGorG3ueo730G4eVchke6zE8uW+IdH30Xdx6EQi7H+pt+"
"zKy1pzDRPptVZ57J1W+5lp/8643gFUjCGjsGJWlGMD40TGN8I42wA8KQ735+gPPecCWDHQGF3pmo"
"wGegkjIyAvkJQb1cw5TrmEBBaLj+j65hINPKSNlQeWoH6fA+QiN5agDaQgik4uq3X8uDFYnddIAf"
"f/LrTIynvOW/XUXhnLVMxFCvpWzdMkT+XIGf7eSGD76Fm/7+m2RKWd704beyI8wihyGMUqfBwkOI"
"ZhlSujF24LH/ZztdGOp5GKMdrFFMX2v7fGb/EyNga02zGXm6JtspgQtrUOMClHTjUS0kgxMYJZFK"
"IUTOJQpwPB8manDth25gS3ERz+0KOSU4xFf/+E8461Vv5Oy/+Bg/eDbigne/m6cfeJqDmzehbcr2"
"UUsFQamjm9lzTkO1tyGSlHNe9VJ26SL7Dxm6hhNEmjAWKSZGYFYqOWXdKQzOnkdLW5Ezr7mE+Zdd"
"zAP7Yhb2Bdz6lf8EKoxVNf37ocekFDI+B+I8h3cZ+rYc5Ilbvg00+EVnhiWLz8S3YG3KL759Fye9"
"7uWsrwnmvP4aPnzZ+ZDN0Dq/naefbeAXcoSxcRosfbdQSjmq+6YFVH7GuV3GuIJEs6Z8VJOPhlbW"
"Ht/kFe/4/Gb7a9PZornF7GQLjTGuwtTaTVIddoV32wSOKUUahaw87TzsOW/gX+9ocMayHHf/w2ew"
"up8nbr6RRS+7nPLs07mt3+ey33sv33j/+7AIBuuWfSMRJ591Ka/6/sXN4SCKclDk6aGI3rmKbd97"
"mLQxRjmBgaqmoj1O/+RfIIVEeYKKhScmoG8GPPzlH/DkrT8BkaNcDdl2QLMwEIwPjnF492F2ZRYz"
"UTqZl//xX5CMjjLvNa/j3i0Rczs8fEKObLyTH3zk77nkz97LYKYVO2MmtgEtP3qMcnEZg6Ucxckz"
"OE1cP1euRFodm9JS0YQnoQJ0qpHCw+Fp7bEOrTj+cuFx1oNN01SI5wXf0lWMhOu3ada9sDYlqY9j"
"reegLUEem9SwJiFXaOO09/8ldz6bp1VA44Gb2XzfTchsH2k4wD2f/kvO/dIPeaacpeOUC1lx9Ruo"
"DB5ElhTFSLFfZ5pDKtw6yBC6ij4j3/spv/zyPyBlSqgyZAsK3cizP3b1CFIIDGSGB3jws9/j8Rv/"
"BZWz6IYm9QpYrUhUnjgNefxf/pZT/+Yk9qYdjK67nkIBZpkROv0MVQ2JFyBEmSd++CWevf1nLL3g"
"JahSG/ufWE9bTjHvL77I6JAlSF1HAtalKHV1rGkJTbPE6FpHSRLaz7qAtFynvuURhNcsJVsxZUGn"
"Awh+59UkKQKkLDaJWAyTw6GN0WRnL8c0GsQj+50pahanhVXYlrmoTCtm7BCYCYw1+JkWetddhI59"
"kJah9f9JEtVBdoKtY9Iqs1a/lOLsBUSppXboINVDO5h79gWksevVsZOz7aXEWKju28PQ9gdAGqTI"
"Meu0synMXIyJQflNxIU01IdHGNi4kbCyzeGOvVZsmtKzeAmF+atI45jBDY8Rjm6irec05lxyJTpb"
"orpnC+WND9O68gzwPEa3PEP1yHaWn38FSaw5vOEJdJjS2t3FmR/4OI/6l1PIKzru/hOe+cnX8YKi"
"gwlhm8fdZEuog0EJILdwBbZWIzqybVql6qgd1XoCY5MTUPAXIPFRqtQUsJ2q71qjKcxbjUkiaoe3"
"IkTGFcSFwOiAnrOuYP66U3j8y19CMgLCQ6cVsKPTPiCP8Gc2TZYFyph0+v8XHTzVjv9PvopCqCxC"
"dbi6bLyv6dXKaTCbqcZmPD+PlW1YlPNO491TyyHULIQ0mGRwWpgyeUxNalGJXGEWZ//lbeyoLaJH"
"HKDNrxPlOnhupJOximRt7yC7PnsZ5ZERpFTNurhtolAskx0p1hqXCEpS1xykXoB41CYkunJcWnxc"
"XJWOECRtwkemfohUPvU9zzp4jRdMG9rhaqNjO7dTOXwIKRvN+m7SbMcoTEGXrGpttr+6wr6liPIs"
"iLRZlM+6xxWFqTqJmIKwNNssRRZE3qFATIJQbc5rtbKpDc0BzigsAYZc02lwSRSp2hEidf6GEGAL"
"KNkFquEQGbh7u7ktEmsyZIotjIyGRBZ2eHNILNgqlHw4rWU3g9/6A8aHduL5MzG64WhRhYcKJMIT"
"6DB1yuO5eUnSa4IojhGi+44Oz3aCTPRU+UlmkbJ49PxFTM0R1vboIEXTLIdJ5WGtIyaRwqUphRc4"
"h8MKp5WBwEYC0dqB7eqGI4PQGG8WNgzW6ObgUycM4+asIoVycjBNcz0ZOxqDNQlSNifENGGoAjMF"
"4LNTz66xSYjwMg5GYx1wwWKRQrlxdDpptuVM8kU2oTQojJ4gyOToWrSWTM9ivEI7UhsaQ/sYeu5+"
"6o3dSDUDYT1EELjNFZXJdrYw55o17Pz244hEOyC9TprubHpsSbZ5XmtdO67Gs99KwEJIPNmKRU0F"
"4+78mMTyHjU5Kt+CDmtNs2uQhTZsmmASBw9FGSeoziL6SAO58mSKH3sv1f/xzzC4C5pjV8Hlc51m"
"NR0SY5GtRVcvLk+4jdT8bBkoV8pLzJSwHHZbPK8iZiFbpHjyampPPQZp4or3OnWVMq1pW9FBoSfH"
"4fv2IzyviYU2U4pkbRVrxn9tNdbz2zA6i0AjvGyTST1yhK6FDCa2bhj0NByQ5fnOlAAbHxf5ym9R"
"Dz5qLgQSKfyjDLRCTgHRRJM004HFPDApzTV1j6Y1Ng0pzmxhwTmLGNreD3XbHG1n0Zv2Yg7uR8TG"
"FSiEdd55rgWbJO7+spnVSRJE4rBhFhDNRjCv0Gw+jyYndwqHxMAifQmpq7O6BmyJ9LKko0MOJ+Up"
"MLppnQw21MTjKUmY4lITjqdENY92Z/5zKOkjlYcQvht1r7KO8NtkHceGAGESLLpJLGoxUYoRXpOi"
"gWlQ2+l/upUzNmxqNidWg50WKzxZAuFP7bxJp0twFMNsrXHISCncKHQbY6WC1KJ8D3zphiLjctNC"
"eNg0QfkZjACbJk3EiHUUQ16Al8ljrMZEdaTwmg3wynVPNDsBSJtfSYlmu6uZosRHCISfRbV0kwzu"
"AuVDqp1gUzsF+JNZH1MNm8hJi/QVOkkozV3Gure/jns/8Umk585K4STtzvIp4F1Kz2WriMdCyk/s"
"RXkCo61DxXqe24yT2Sk7OYXKToWYU5QNgDURqan9VsXg30KDJwHSTiCi6ZkKMaUszZedag1xn+Ta"
"LaUVFOd0E9djTJROAQVktgV0gsoHWGWwiRvQqLp6EGi8VXPRQxMOgK5Dl6wXPtm5J4HxMI0hhLVY"
"k6LyPiIbYMImyrNJluoQkAJsim2UHTwXEJ6PMTG5dfNdtW686vDXApDNrgthQTkwoAo042kVGhq/"
"LY9tJM4RnNQ64baVV8ihAp/w8IizHq0zaV16GvHICIL4qD8/FS6Z51UTRLNdNGxmwY//kvyWlzEx"
"xjSazzI9jGim5dAgUhDaIRN0CMagigGzz13lppBI4/oxhIW0AaZBcPoqZFvJVV2kQY8Pojrz+J0F"
"dw+a7RzCYq2gtORkvJYWV04EJ8w0xcZxUwOaPbyA8pwDJrSbdIoK3FyiJCRoa0UPV9AjY84aNLVx"
"Eoyusj65UpbGcD9bbrkZr7UDv3c2yeBIU2cdWYoAhDFI6VHesI/RR7e7Sd8mRfkeLXPnHptnnprJ"
"bH+loCPQGBNijyPu/Z2Y6OntLEJmHEkp3q+5lT0KjW1eOtUoJZtQEXd+2klwmZSYNHWVlinQpm2W"
"2bLHxrxCYoyHkNqVHoVCZbOY2rhzkiZNnDYExRyFWe2MbT/c9A1wmG0Tu/Swn8VENQymGVIx5XEL"
"0eyRctOtUJkSplbFqknYUto0ahrhe3ilEsnY2LT7TEYWYI3E92WzKmSmdSs8LxUsUowJMSY57tDo"
"dybgSRdeigxS5FzI82sKllPxKhYp5VT6zSmJwDT/KafgudPm1k/afeM+z0x7chcdmaab4prXpPPw"
"nLCOqXoZx7pDc4KbORpDT2WWppfImptSPn/JrHEbsMnE3vTwmh6wQCqvWRacHubIJiAxPXr7qXWx"
"x3weJBjTwBwHq+wJE/BRhIdCyhxSZH5NJerYuM4+b1SbtWJK44TVzXCkmcCYZrmmYu+pzWGPSdoj"
"5HQwkcMkGzetfOo9z0MT2mbq0DXU/a/q4dMPONVMgByLW37+ogrRzA4I5b7br6uzW+ctGxs1z3T7"
"f4uAp5vsACmCqeas/5mwXTjl4korBNK6kMi1jXKsdr3QvezRz6UpzOmfKZAYKR1b3JSbIH7FWbST"
"yZpjGz5f4AvaKXMrEKCajW/PA0EcU0IV4lcdp1/ZLAZrY+fX2Ph3KZLfrYCn+25SeM7LFkHTU27G"
"y/bYL2el82mUEAhrCQpZkkbkEhXTVNzao8I5akHtlNCmenomtcUetRRgjsK5OVaTp8tbTFsQMdXF"
"Me13hJ76iUBghGzWt+2vbODJsMxyrLmfgh+jXVciCcYkTUfqdy+KEyTgY+DyLos02SQu5NHqlJz8"
"eXMJp7KNdhrW+ledtGNM8jFGzk6TzrQOgOb5b5sFa2eSxRTo6Oix0XR4Jo+FY87FX7Ns4lfhU9OP"
"I4tpxvFmMonrHCybYkl/K6Tk/1UC/l8hMyeNon2e5XrB+U+TGTHxAmZuqrtC/Aqs6AWdvibcd1r+"
"4+jxcYzjNP0Wv2ZnAYJfeyI1M3n2ePFy/68L+MXr/8QlX1yCFwX84vWigF+8XhTwi9eLAn7xelHA"
"L14vCvjF60UBvyjgF6//Kpf34hL817wmU7b/H8g2kndIXRj3AAAAAElFTkSuQmCC";

static const char* s_img_tr =
"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"
"rkJggg==";

static const char* s_img_d83 =
"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";

static const char* s_img_dc =
"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";

// ============================================================================
// BuildAbout
// ============================================================================

static void BuildAbout()
{
    const char* ver = Update_GetLocalVersion();
    PageStart("About", "ab");

    BA("<div class=about-hero>"
        "<img src='data:image/png;base64,");
    BA(k_img_xbdiag);
    BA("' alt='XbDiag' style='height:120px;width:auto'>"
        "<div style='margin-top:16px;font-size:24px;font-weight:600;"
        "color:#E8ECFF;letter-spacing:.5px'>XbDiag</div>"
        "<div style='font-size:13px;color:#5A6278;margin-top:4px'>"
        "Original Xbox Hardware Diagnostic Suite</div>"
        "</div>");

    BA("<div class=card style='max-width:480px;margin:0 auto 24px'>"
        "<div class=card-head>"
        "<span class=card-icon style='background:#50DCFF'></span>"
        "<span class=card-title>Version</span></div>"
        "<div class=card-body>");
    BA("<div class=row><div class=lbl>Build</div>"
        "<div class='val cy'>");
    BA(ver ? ver : "1.0.8 Beta");
    BA("</div></div>"
        "<div class=row><div class=lbl>Platform</div>"
        "<div class=val>Original Xbox (RXDK)</div></div>"
        "<div class=row><div class=lbl>Web Interface</div>"
        "<div class=val>HTTP/1.0 &bull; Port 80</div></div>"
        "</div></div>");

    BA("<div class=info style='max-width:640px;margin:0 auto 32px'>"
        "XbDiag is a native hardware diagnostic suite for original Xbox consoles "
        "(revisions 1.0 through 1.6). It runs directly on the hardware with no "
        "dependencies outside the Xbox kernel, providing deep visibility into CPU, "
        "memory, SMBus devices, temperatures, EEPROM, HDD, video, and controller "
        "state. This web dashboard is served directly from the Xbox over port 80."
        "</div>");

    BA("<div style='text-align:center;margin-bottom:8px;font-size:10px;"
        "letter-spacing:2px;color:#2A3040;text-transform:uppercase'>"
        "Built by</div>");
    BA("<div class=about-logos>");

    BA("<div style='text-align:center'>"
        "<img src='data:image/png;base64,");
    BA(k_img_tr);
    BA("' alt='Team Resurgent' style='height:80px;width:auto;opacity:.9'>"
        "</div>");

    BA("<div style='text-align:center'>"
        "<img src='data:image/png;base64,");
    BA(k_img_d83);
    BA("' alt='Darkone83' style='height:80px;width:auto;opacity:.9'>"
        "</div>");

    BA("<div style='text-align:center'>"
        "<img src='data:image/png;base64,");
    BA(k_img_dc);
    BA("' alt='Darkone Customs' style='height:70px;width:auto;opacity:.9'>"
        "</div>");

    BA("</div>"); // about-logos
    PageEnd();
}

// ============================================================================
// HTTP helpers
// ============================================================================

static bool ParseRequest(const char* buf, int len,
    char* outMethod, int methodLen,
    char* outPath, int pathLen,
    char* outQuery, int queryLen)
{
    int mi = 0;
    int pi = 0;
    int qi = 0;

    outMethod[0] = outPath[0] = outQuery[0] = '\0';
    if (len < 5) return false;

    // Method
    while (mi < len && buf[mi] != ' ' && mi < methodLen - 1)
        outMethod[mi] = buf[mi++];
    outMethod[mi] = '\0';
    if (buf[mi] != ' ') return false;
    ++mi;

    // Path + query
    while (mi < len && buf[mi] != ' ' && buf[mi] != '\r' && buf[mi] != '\n')
    {
        if (buf[mi] == '?' && outQuery[0] == '\0')
        {
            outPath[pi] = '\0';
            ++mi;
            qi = 0;
            while (mi < len && buf[mi] != ' ' && buf[mi] != '\r' && qi < queryLen - 1)
                outQuery[qi++] = buf[mi++];
            outQuery[qi] = '\0';
            return true;
        }
        if (pi < pathLen - 1) outPath[pi++] = buf[mi];
        ++mi;
    }
    outPath[pi] = '\0';
    return true;
}

static int ParseContentLength(const char* buf, int len)
{
    // Case-insensitive search for content-length header
    int i, j, k;
    for (i = 0; i < len - 16; ++i)
    {
        // Match "content-length: " case-insensitively
        bool m = true;
        const char* needle = "content-length: ";
        for (j = 0; j < 16 && m; ++j)
        {
            char c = buf[i + j];
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
            if (c != needle[j]) m = false;
        }
        if (m)
        {
            int val = 0;
            for (k = i + 16; k < len && buf[k] >= '0' && buf[k] <= '9'; ++k)
                val = val * 10 + (buf[k] - '0');
            return val;
        }
    }
    return 0;
}

static void SendHTML(SOCKET c)
{
    char hdr[256];
    char cl[16];
    int sent = 0;
    const int chunkSize = 4096;

    IntToStr(s_bufLen, cl, sizeof(cl));
    StrCopy(hdr, sizeof(hdr),
        "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: ");
    StrCat2(hdr, sizeof(hdr), hdr, cl);
    StrCat2(hdr, sizeof(hdr), hdr, "\r\nConnection: close\r\n\r\n");
    send(c, hdr, StrLen(hdr), 0);

    // Send in 4KB chunks -- large pages (About ~82KB) won't fit in one send()
    while (sent < s_bufLen)
    {
        int toSend = s_bufLen - sent;
        int n;
        if (toSend > chunkSize) toSend = chunkSize;
        n = send(c, s_buf + sent, toSend, 0);
        if (n <= 0) break;
        sent += n;
    }
}

static void Send303(SOCKET c, const char* loc)
{
    char r[256];
    StrCopy(r, sizeof(r), "HTTP/1.1 303 See Other\r\nLocation: ");
    StrCat2(r, sizeof(r), r, loc);
    StrCat2(r, sizeof(r), r, "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    send(c, r, StrLen(r), 0);
}

static void Send301(SOCKET c, const char* loc)
{
    char r[256];
    StrCopy(r, sizeof(r), "HTTP/1.0 301 Moved Permanently\r\nLocation: ");
    StrCat2(r, sizeof(r), r, loc);
    StrCat2(r, sizeof(r), r, "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    send(c, r, StrLen(r), 0);
}

static void Send404(SOCKET c)
{
    const char* r =
        "HTTP/1.0 404 Not Found\r\nContent-Length: 9\r\n"
        "Connection: close\r\n\r\nNot found";
    send(c, r, StrLen(r), 0);
}

static void SendBinary(SOCKET c, const char* fname)
{
    char path[64];
    char szBuf[16];
    char hdr[512];
    char chunk[4096];
    HANDLE hf;
    DWORD fsize;
    DWORD nr;
    bool aborted;

    StrCopy(path, sizeof(path), "D:\\");
    StrCat2(path, sizeof(path), path, fname);

    hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { Send404(c); return; }

    fsize = GetFileSize(hf, NULL);
    IntToStr((int)fsize, szBuf, sizeof(szBuf));

    StrCopy(hdr, sizeof(hdr),
        "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"");
    StrCat2(hdr, sizeof(hdr), hdr, fname);
    StrCat2(hdr, sizeof(hdr), hdr, "\"\r\nContent-Length: ");
    StrCat2(hdr, sizeof(hdr), hdr, szBuf);
    StrCat2(hdr, sizeof(hdr), hdr, "\r\nConnection: close\r\n\r\n");
    send(c, hdr, StrLen(hdr), 0);

    // Stream file in 4KB chunks
    nr = 0;
    aborted = false;
    while (!aborted && ReadFile(hf, chunk, sizeof(chunk), &nr, NULL) && nr > 0)
    {
        int sent = 0;
        while (sent < (int)nr)
        {
            int n = send(c, chunk + sent, (int)nr - sent, 0);
            if (n <= 0) { aborted = true; break; }
            sent += n;
        }
    }
    CloseHandle(hf);
}

static void ServeClient(SOCKET c)
{
    int tmo = 5000;
    u_long nb = 0;
    char req[4096];
    int rlen = 0;
    char method[8], path[128], query[512];
    int i;

    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));

    ioctlsocket(c, FIONBIO, &nb);

    // Receive headers
    {
        bool hdrdone = false;
        while (!hdrdone && rlen < (int)sizeof(req) - 1)
        {
            int n = recv(c, req + rlen, (int)sizeof(req) - 1 - rlen, 0);
            if (n <= 0) break;
            rlen += n; req[rlen] = '\0';
            for (i = 0; i < rlen - 3 && !hdrdone; ++i)
            {
                if (req[i] == '\r' && req[i + 1] == '\n' &&
                    req[i + 2] == '\r' && req[i + 3] == '\n')
                    hdrdone = true;
            }
        }
    }

    if (rlen < 5) { closesocket(c); return; }

    if (!ParseRequest(req, rlen, method, sizeof(method),
        path, sizeof(path), query, sizeof(query)))
    {
        Send404(c); closesocket(c); return;
    }

    // Route
    if (SEq(path, "/") || SEq(path, ""))
    {
        Send301(c, "/sysinfo");
    }
    else if (SEq(path, "/sysinfo"))
    {
        BuildSysInfo(); SendHTML(c);
    }
    else if (SEq(path, "/report"))
    {
        char fname[64];
        GetParam(query, "f", fname, sizeof(fname));
        BuildReport(fname[0] ? fname : NULL);
        SendHTML(c);
    }
    else if (SEq(path, "/settings"))
    {
        BuildSettings(); SendHTML(c);
    }
    else if (SEq(path, "/save"))
    {
        // Settings save via GET query string -- avoids HTTP/1.0 POST limitations
        s_saveOK = SaveSettingsFromPost(query);
        s_saveAttempted = true;
        Send301(c, "/settings");
    }
    else if (SEq(path, "/about"))
    {
        BuildAbout(); SendHTML(c);
    }
    else if (SEq(path, "/download"))
    {
        char fname[64]; GetParam(query, "f", fname, sizeof(fname));
        if (fname[0]) SendBinary(c, fname);
        else Send404(c);
    }
    else
    {
        Send404(c);
    }

    closesocket(c);
}

// ============================================================================
// Network helpers -- mirrors FtpServ_Start socket pattern exactly
// ============================================================================

static void DiscoverLocalIP()
{
    SOCKET s;
    sockaddr_in remote;
    sockaddr_in local;
    int len;
    BYTE* b;
    char oc[5];
    char dot[2];

    dot[0] = '.'; dot[1] = '\0';
    s_localIP[0] = '\0';

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;

    ZeroMemory(&remote, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(80);
    remote.sin_addr.s_addr = inet_addr("8.8.8.8");

    if (connect(s, (sockaddr*)&remote, sizeof(remote)) != 0)
    {
        closesocket(s); return;
    }

    ZeroMemory(&local, sizeof(local));
    len = sizeof(local);
    if (getsockname(s, (sockaddr*)&local, &len) != 0)
    {
        closesocket(s); return;
    }
    closesocket(s);

    b = (BYTE*)&local.sin_addr.s_addr;
    IntToStr(b[0], oc, sizeof(oc)); StrCopy(s_localIP, sizeof(s_localIP), oc);
    StrCat2(s_localIP, sizeof(s_localIP), s_localIP, dot);
    IntToStr(b[1], oc, sizeof(oc)); StrCat2(s_localIP, sizeof(s_localIP), s_localIP, oc);
    StrCat2(s_localIP, sizeof(s_localIP), s_localIP, dot);
    IntToStr(b[2], oc, sizeof(oc)); StrCat2(s_localIP, sizeof(s_localIP), s_localIP, oc);
    StrCat2(s_localIP, sizeof(s_localIP), s_localIP, dot);
    IntToStr(b[3], oc, sizeof(oc)); StrCat2(s_localIP, sizeof(s_localIP), s_localIP, oc);
}

static bool BindListen()
{
    SOCKET s;
    DWORD reuse = 1;
    u_long nb = 1;
    SOCKADDR_IN sa;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    ioctlsocket(s, FIONBIO, &nb);

    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(80);

    if (bind(s, (SOCKADDR*)&sa, sizeof(sa)) != 0 ||
        listen(s, 5) != 0)
    {
        closesocket(s); return false;
    }

    s_listenSock = s;
    return true;
}

// ============================================================================
// Public API
// ============================================================================

void HttpRptSrv_Start()
{
    // Assign image pointers -- s_img_* are defined after PageStart in this TU
    k_img_xbdiag = s_img_xbdiag;
    k_img_tr = s_img_tr;
    k_img_d83 = s_img_d83;
    k_img_dc = s_img_dc;

    s_started = false;
    s_bound = false;
    s_localIP[0] = '\0';
    s_listenSock = INVALID_SOCKET;
    s_saveAttempted = false;
    s_saveOK = false;

    if (!BindListen()) return;
    DiscoverLocalIP();
    s_bound = true;
    s_started = true;
}

void HttpRptSrv_Poll()
{
    if (!s_started || !s_bound) return;
    if (s_listenSock == INVALID_SOCKET) return;

    fd_set rset; FD_ZERO(&rset); FD_SET(s_listenSock, &rset);
    TIMEVAL tv = { 0, 0 };
    if (select(0, &rset, NULL, NULL, &tv) != 1) return;

    sockaddr_in caddr; int cl = sizeof(caddr);
    SOCKET c = accept(s_listenSock, (sockaddr*)&caddr, &cl);
    if (c == INVALID_SOCKET) return;

    ServeClient(c);
}